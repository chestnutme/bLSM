/*
 * check_mergetuple.cpp
 *
 * Copyright 2009-2012 Yahoo! Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *      Author: makdere
 */
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <algorithm>
#include "bLSM.h"
#include "dataPage.h"
#include "mergeScheduler.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

#include <stasis/transactional.h>

#include "check_util.h"

void insertProbeIter(size_t NUM_ENTRIES)
{
    srand(1000);
    unlink("storefile.txt");
    unlink("logfile.txt");
    system("rm -rf stasis_log/");

    sync();

    bLSM::init_stasis();

    double delete_freq = .05;
    double update_freq = .15;

    //data generation
    typedef std::vector<std::string> key_v_t;
    const static size_t max_partition_size = 100000;
    int KEY_LEN = 100;
    std::vector<key_v_t*> *key_v_list = new std::vector<key_v_t*>;
    int list_size = NUM_ENTRIES / max_partition_size + 1;
    for(int i =0; i<list_size; i++)
    {
        key_v_t * key_arr = new key_v_t;
        if(NUM_ENTRIES < max_partition_size*(i+1))
            preprandstr(NUM_ENTRIES-max_partition_size*i, key_arr, KEY_LEN, true);
        else
            preprandstr(max_partition_size, key_arr, KEY_LEN, true);
    
        std::sort(key_arr->begin(), key_arr->end(), &mycmp);
        key_v_list->push_back(key_arr);
        printf("size partition %llu is %llu\n", (unsigned long long)i+1, (unsigned long long)key_arr->size());
    }

    key_v_t * key_arr = new key_v_t;
    
    std::vector<key_v_t::iterator*> iters;
    for(int i=0; i<list_size; i++)
    {
        iters.push_back(new key_v_t::iterator((*key_v_list)[i]->begin()));
    }

    size_t lc = 0;
    while(true)
    {
        int list_index = -1;
        for(int i=0; i<list_size; i++)
        {
            if(*iters[i] == (*key_v_list)[i]->end())
                continue;
            
            if(list_index == -1 || mycmp(**iters[i], **iters[list_index]))
                list_index = i;
        }

        if(list_index == -1)
            break;
        
        if(key_arr->size() == 0 || mycmp(key_arr->back(), **iters[list_index]))
            key_arr->push_back(**iters[list_index]);

        (*iters[list_index])++;        
        lc++;
        if(lc % max_partition_size == 0)
            printf("%llu/%llu completed.\n", (unsigned long long)lc, (unsigned long long)NUM_ENTRIES);
    }

    for(int i=0; i<list_size; i++)
    {
        (*key_v_list)[i]->clear();
        delete (*key_v_list)[i];
        delete iters[i];
    }
    key_v_list->clear();
    delete key_v_list;
    
    printf("key arr size: %llu\n", (unsigned long long)key_arr->size());

    if(key_arr->size() > NUM_ENTRIES)
        key_arr->erase(key_arr->begin()+NUM_ENTRIES, key_arr->end());
    
    NUM_ENTRIES=key_arr->size();
    
    int xid = Tbegin();

    bLSM *ltable = new bLSM(10 * 1024 * 1024, 1000, 1000, 40);
    mergeScheduler mscheduler(ltable);

    recordid table_root = ltable->allocTable(xid);

    Tcommit(xid);

    mscheduler.start();

    printf("Stage 1: Writing %llu keys\n", (unsigned long long)NUM_ENTRIES);
    
    struct timeval start_tv, stop_tv, ti_st, ti_end;
    double insert_time = 0;
    int delcount = 0, upcount = 0;
    int64_t datasize = 0;
    std::vector<pageid_t> dsp;
    std::vector<int> del_list;
    gettimeofday(&start_tv,0);
    for(size_t i = 0; i < NUM_ENTRIES; i++)
    {
        //prepare the data
        std::string ditem;
        getnextdata(ditem, 8192);

        //prepare the key
        dataTuple *newtuple = dataTuple::create((*key_arr)[i].c_str(), (*key_arr)[i].length()+1, ditem.c_str(), ditem.length()+1);
        
        datasize += newtuple->byte_length();

        gettimeofday(&ti_st,0);        
        ltable->insertTuple(newtuple);
        gettimeofday(&ti_end,0);
        insert_time += tv_to_double(ti_end) - tv_to_double(ti_st);

        dataTuple::freetuple(newtuple);

        double rval = ((rand() % 100)+.0)/100;        
        if( rval < delete_freq) //delete a key 
        {
            int del_index = i - (rand()%50); //delete one of the last inserted 50 elements
            if(del_index >= 0 && std::find(del_list.begin(), del_list.end(), del_index) == del_list.end())
            {
                delcount++;

                dataTuple *deltuple = dataTuple::create((*key_arr)[del_index].c_str(), (*key_arr)[del_index].length()+1);

                gettimeofday(&ti_st,0);        
                ltable->insertTuple(deltuple);
                gettimeofday(&ti_end,0);
                insert_time += tv_to_double(ti_end) - tv_to_double(ti_st);

                dataTuple::freetuple(deltuple);

                del_list.push_back(del_index);
                
            }            
        }
        else if(rval < delete_freq + update_freq) //update a record
        {
            int up_index = i - (rand()%50); //update one of the last inserted 50 elements
            if(up_index >= 0 && std::find(del_list.begin(), del_list.end(), up_index) == del_list.end()) 
            {//only update non-deleted elements
                getnextdata(ditem, 512);

                upcount++;
                dataTuple *uptuple = dataTuple::create((*key_arr)[up_index].c_str(), (*key_arr)[up_index].length()+1,
													   ditem.c_str(), ditem.length()+1);
                gettimeofday(&ti_st,0);        
                ltable->insertTuple(uptuple);
                gettimeofday(&ti_end,0);
                insert_time += tv_to_double(ti_end) - tv_to_double(ti_st);

                dataTuple::freetuple(uptuple);
            }            

        }
        
    }
    gettimeofday(&stop_tv,0);
    printf("insert time: %6.1f\n", insert_time);
    printf("insert time: %6.1f\n", (tv_to_double(stop_tv) - tv_to_double(start_tv)));
    printf("#deletions: %d\n#updates: %d\n", delcount, upcount);

    printf("\nTREE STRUCTURE\n");
    printf("datasize: %llu\n", (unsigned long long)datasize);

    xid = Tbegin();



    printf("Stage 2: Looking up %llu keys:\n", (unsigned long long)NUM_ENTRIES);

    int found_tuples=0;
    for(int i=NUM_ENTRIES-1; i>=0; i--)
    {        
        int ri = i;

        //get the key
        uint32_t keylen = (*key_arr)[ri].length()+1;        
        dataTuple::key_t rkey = (dataTuple::key_t) malloc(keylen);
        memcpy((byte*)rkey, (*key_arr)[ri].c_str(), keylen);

        //find the key with the given tuple
        dataTuple *dt = ltable->findTuple(xid, rkey, keylen);

        if(std::find(del_list.begin(), del_list.end(), i) == del_list.end())
        {
            assert(dt!=0);
            assert(!dt->isDelete());
            found_tuples++;
            assert(dt->rawkeylen() == (*key_arr)[ri].length()+1);
            dataTuple::freetuple(dt);
        }
        else
        {
            if(dt!=0)
            {
                assert(dt->rawkeylen() == (*key_arr)[ri].length()+1);
                assert(dt->isDelete());
                dataTuple::freetuple(dt);
            }
        }
        dt = 0;
        free(rkey);
    }
    printf("found %d\n", found_tuples);




    
    key_arr->clear();
    //data_arr->clear();
    delete key_arr;
    //delete data_arr;
    
    mscheduler.shutdown();
    printf("merge threads finished.\n");
    gettimeofday(&stop_tv,0);
    printf("run time: %6.1f\n", (tv_to_double(stop_tv) - tv_to_double(start_tv)));


    
    Tcommit(xid);
    delete ltable;
    bLSM::deinit_stasis();
}



/** @test
 */
int main()
{
//    insertProbeIter(25000);
    insertProbeIter(400000);
    /*
    insertProbeIter(5000);
    insertProbeIter(2500);
    insertProbeIter(1000);
    insertProbeIter(500);
    insertProbeIter(1000);
    insertProbeIter(100);
    insertProbeIter(10);
    */
    
    return 0;
}

