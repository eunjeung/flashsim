/* Copyright 2011 Matias Bjørling */

/* dftp_ftl.cpp  */

/* FlashSim is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version. */

/* FlashSim is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. */

/* You should have received a copy of the GNU General Public License
 * along with FlashSim.  If not, see <http://www.gnu.org/licenses/>. */

/****************************************************************************/

/* Implementation of the DFTL described in the paper
 * "DFTL: A Flasg Translation Layer Employing Demand-based Selective Caching og Page-level Address Mappings"
 *
 * Global Mapping Table GMT
 * Global Translation Directory GTD (Maintained in memory)
 * Cached Mapping Table CMT (Uses LRU to pick victim)
 *
 * Dlpn/Dppn Data Logical/Physical Page Number
 * Mlpn/Mppn Translation Logical/Physical Page Number
 */


#include <new>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <queue>
#include <iostream>
#include <limits>
#include "../ssd.h"

using namespace ssd;

FtlImpl_DftlParent::MPage::MPage(long vpn)
{
	this->vpn = vpn;
	this->ppn = -1;
	this->create_ts = -1;
	this->modified_ts = -1;
}

double FtlImpl_DftlParent::mpage_modified_ts_compare(const FtlImpl_DftlParent::MPage& mpage)
{
	if (mpage.modified_ts == -1 || !mpage.cached)
		return std::numeric_limits<double>::max();

	return mpage.modified_ts;
}

FtlImpl_DftlParent::FtlImpl_DftlParent(Controller &controller):
	FtlParent(controller)
{
	addressPerPage = 0;

	currentDataPage = -1;
	currentTranslationPage = -1;

	// Detect required number of bits for logical address size
	addressSize = log(SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * BLOCK_SIZE)/log(2);

	// Find required number of bits for block size
	addressPerPage = (PAGE_SIZE/ceil(addressSize / 8.0)); // 8 bits per byte

	printf("Total required bits for representation: Address size: %i Total per page: %i \n", addressSize, addressPerPage);

	totalCMTentries = CACHE_DFTL_LIMIT * addressPerPage;
	printf("Number of elements in Cached Mapping Table (CMT): %i\n", totalCMTentries);

	// Initialise block mapping table.
	uint ssdSize = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * BLOCK_SIZE;

	//trans_map = new MPage[ssdSize];
	for (uint i=0;i<ssdSize;i++)
	{
		trans_map.insert(MPage(i));
	}

	reverse_trans_map = new long[ssdSize];
}

void FtlImpl_DftlParent::consult_GTD(long dlpn, Event &event)
{
	// Simulate that we goto translation map and read the mapping page.
	Event readEvent = Event(READ, event.get_logical_address(), 1, event.get_start_time());
	readEvent.set_address(Address(1, PAGE));
	readEvent.set_noop(true);

	if (controller.issue(readEvent) == FAILURE) { assert(false);}
	//event.consolidate_metaevent(readEvent);
	event.incr_time_taken(readEvent.get_time_taken());
	controller.stats.numFTLRead++;
}

void FtlImpl_DftlParent::reset_MPage(FtlImpl_DftlParent::MPage &mpage)
{
	mpage.create_ts = -1;
	mpage.modified_ts = -1;
}

bool FtlImpl_DftlParent::lookup_CMT(long dlpn, Event &event)
{
	if (cmt.find(dlpn) == cmt.end())
		return false;

	event.incr_time_taken(RAM_READ_DELAY);
	controller.stats.numMemoryRead++;

	return true;
}

long FtlImpl_DftlParent::get_free_translation_page()
{
	if (currentTranslationPage == -1 || currentTranslationPage % BLOCK_SIZE == BLOCK_SIZE -1)
		currentTranslationPage = manager.get_free_block(LOG).get_linear_address();
	else
		currentTranslationPage++;

	return currentTranslationPage;
}

long FtlImpl_DftlParent::get_free_data_page()
{
	if (currentDataPage == -1 || currentDataPage % BLOCK_SIZE == BLOCK_SIZE -1)
		currentDataPage = manager.get_free_block(DATA).get_linear_address();
	else
		currentDataPage++;

	return currentDataPage;
}

FtlImpl_DftlParent::~FtlImpl_DftlParent(void)
{
	delete[] reverse_trans_map;
}

void FtlImpl_DftlParent::resolve_mapping(Event &event, bool isWrite)
{
	uint dlpn = event.get_logical_address();
	/* 1. Lookup in CMT if the mapping exist
	 * 2. If, then serve
	 * 3. If not, then goto GDT, lookup page
	 * 4. If CMT full, evict a page
	 * 5. Add mapping to CMT
	 */
	if (lookup_CMT(event.get_logical_address(), event))
	{
		controller.stats.numCacheHits++;

		if (isWrite)
		{
			MpageByID::iterator it = trans_map.find(dlpn);
			MPage current = *it;
			current.modified_ts = event.get_start_time();
			trans_map.replace(it, current);

			// Inform the ssd model that it should invalidate the previous page.
			Address killAddress = Address(current.ppn, PAGE);
			event.set_replace_address(killAddress);
		}
	} else {
		controller.stats.numCacheFaults++;
		consult_GTD(dlpn, event);

		MpageByID::iterator it = trans_map.find(dlpn);
		MPage current = *it;
		current.modified_ts = event.get_start_time();
		current.create_ts = event.get_start_time();
		current.cached = true;

		cmt[dlpn] = true;

		if (cmt.size() == totalCMTentries)
		{
			// Find page to evict
			MpageByModified::iterator evictit = boost::multi_index::get<1>(trans_map).begin();
			MPage evictPage = *++evictit;
			//printf("Im here3: %li %li %f %f\n", evictPage.vpn, evictPage.ppn, evictPage.create_ts, evictPage.modified_ts);
			MpageByID::iterator it = trans_map.find(evictPage.vpn);

			if (evictPage.create_ts != evictPage.modified_ts)
			{
				// Evict page
				// Inform the ssd model that it should invalidate the previous page.
				// Calculate the start address of the translation page.
				int vpnBase = evictPage.vpn - evictPage.vpn % addressPerPage;

				for (int i=vpnBase;i<vpnBase+addressPerPage;i++)
				{
					MpageByID::iterator cit = trans_map.find(i);
					MPage cur = *cit;
					cur.create_ts = cur.modified_ts;
					trans_map.replace(cit, cur);
				}


				// Simulate the write to translate page
				Event write_event = Event(WRITE, event.get_logical_address(), 1, event.get_start_time());
				write_event.set_address(Address(0, PAGE));
				write_event.set_noop(true);

				if (controller.issue(write_event) == FAILURE) {	assert(false);}

				event.incr_time_taken(write_event.get_time_taken());
				//event.consolidate_metaevent(write_event);
			}

			// Remove page from cache.
			cmt.erase(evictPage.vpn);
			evictPage.cached = false;

			reset_MPage(evictPage);
			trans_map.replace(it, evictPage);
		}


		trans_map.replace(it, current);
	}
}

void FtlImpl_DftlParent::update_translation_map(FtlImpl_DftlParent::MPage &mpage, long ppn)
{
	mpage.ppn = ppn;
	reverse_trans_map[ppn] = mpage.vpn;
}
