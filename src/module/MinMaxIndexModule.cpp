// -*-C++-*-
/*
  (c) Copyright 2004-2005, Hewlett-Packard Development Company, LP

  See the file named COPYING for license details
*/

/** @file
    implementation
*/

#include <algorithm>

#include <Lintel/HashMap.hpp>

#include <DataSeries/MinMaxIndexModule.hpp>
#include <DataSeries/TypeIndexModule.hpp>

using namespace std;
using boost::format;

static bool inrange(const GeneralValue &v, const GeneralValue &minrange,
                    const GeneralValue &maxrange)
{
    return minrange <= v && v <= maxrange;
}

static bool intervalOverlap(const GeneralValue &a_min, const GeneralValue &a_max,
                            const GeneralValue &b_min, const GeneralValue &b_max)
{
    return inrange(a_min,b_min,b_max) ||
            inrange(a_max,b_min,b_max) ||
            inrange(b_min, a_min, a_max) ||
            inrange(b_max, a_min, a_max);
}

void
MinMaxIndexModule::init(const std::string &index_filename,
                        std::vector<selector> &intersection_list,
                        const std::string &sort_fieldname)
{
    TypeIndexModule tim("DSIndex::Extent::MinMax::" + index_type);
    tim.addSource(index_filename);

    ExtentSeries s;
    Variable32Field filename(s,"filename");
    Int64Field extent_offset(s,"extent_offset");
    GeneralField *sort_val = NULL;

    while (true) {
        Extent::Ptr e = tim.getSharedExtent();
        if (e == NULL) {
            break;
        }
        s.setExtent(e);
        if (sort_val == NULL) {
            sort_val = GeneralField::create(NULL,s,sort_fieldname);
            // can't create generalfields until we know the type, but
            // can't know the type until we read in the extent.  We could
            // modify generalfields so that their type can be determined
            // based on a generalvalue type, but that would be limiting.
            for (unsigned i=0;i<intersection_list.size();++i) {
                selector &sel = intersection_list[i];
                sel.minf = GeneralField::create(NULL,s,"min:" + sel.min_fieldname);
                sel.maxf = GeneralField::create(NULL,s,"max:" + sel.max_fieldname);
            }
        }
        for (;s.morerecords();++s) {
            bool all_overlap = true;
            bool any_overlap = false;
            GeneralValue extent_sort(sort_val);
            for (unsigned i=0;i<intersection_list.size();++i) {
                selector &sel = intersection_list[i];
                GeneralValue extent_min(sel.minf);
                GeneralValue extent_max(sel.maxf);

                if (false == intervalOverlap(extent_min,extent_max,sel.minv,sel.maxv)) {
                    all_overlap = false;
                    any_overlap |= false;
                    if (!use_or) {
                        break;
                    }
                } else {
                    any_overlap |= true;
                }
            }
            if (!use_or) {
                if (all_overlap) {
                    if (false) {
                        cout << format("keep %s @ %d\n")
                                % filename.stringval() % extent_offset.val();
                    }
                    kept_extents.push_back(kept_extent(filename.stringval(),
                                                       extent_offset.val(),
                                                       extent_sort));
                } else {
                    if (false) {
                        cout << format("skip %s @ %d\n")
                                % filename.stringval() % extent_offset.val();
                    }
                }
            } else {
                if (any_overlap) {
                    if (false) {
                        cout << format("keep %s @ %d\n")
                                % filename.stringval() % extent_offset.val();
                    }
                    kept_extents.push_back(kept_extent(filename.stringval(),
                                                       extent_offset.val(),
                                                       extent_sort));
                } else {
                    if (false) {
                        cout << format("skip %s @ %d\n")
                                % filename.stringval() % extent_offset.val();
                    }
                }
            }
        }
    }
    for (unsigned i=0;i<intersection_list.size();++i) {
        selector &sel = intersection_list[i];
        delete sel.minf;
        delete sel.maxf;
    }
    delete sort_val;
    sort(kept_extents.begin(),kept_extents.end(),kept_extent_bysortvalue());
}


MinMaxIndexModule::MinMaxIndexModule(const string &index_filename,
                                     const string &_index_type,
                                     const GeneralValue minv, 
                                     const GeneralValue maxv,
                                     const string &min_fieldname,
                                     const string &max_fieldname,
                                     const string &sort_fieldname)
        : IndexSourceModule(), index_type(_index_type), 
          cur_extent(0), cur_source(NULL), use_or(false)
{
    vector<selector> tmp;
    selector foo(minv,maxv,min_fieldname,max_fieldname);
    tmp.push_back(foo);
    init(index_filename,tmp,sort_fieldname);
}

MinMaxIndexModule::MinMaxIndexModule(const std::string &index_filename,
                                     const std::string &_index_type,
                                     std::vector<selector> intersection_list,
                                     const std::string &sort_fieldname)
        : IndexSourceModule(), index_type(_index_type), 
          cur_extent(0), cur_source(NULL), use_or(false)
{
    init(index_filename,intersection_list,sort_fieldname);
}

MinMaxIndexModule::MinMaxIndexModule(const std::string &index_filename,
                                     const std::string &_index_type,
                                     std::vector<selector> intersection_list,
                                     const std::string &sort_fieldname,
                                     const bool _use_or)
        : IndexSourceModule(), index_type(_index_type), 
          cur_extent(0), cur_source(NULL), use_or(_use_or)
{
    init(index_filename,intersection_list,sort_fieldname);
}


void
MinMaxIndexModule::lockedResetModule()
{
    FATAL_ERROR("unimplemented");
}

IndexSourceModule::PrefetchExtent *
MinMaxIndexModule::lockedGetCompressedExtent()
{
    if (cur_extent >= kept_extents.size()) {
        delete cur_source;
        cur_source = NULL;
        cur_source_filename.clear();
        return NULL;
    }
    if (cur_source_filename != kept_extents[cur_extent].filename) {
        delete cur_source;
        cur_source_filename = kept_extents[cur_extent].filename;
        cur_source = new DataSeriesSource(cur_source_filename);
    }
    PrefetchExtent *ret = 
            readCompressed(cur_source,
                           kept_extents[cur_extent].extent_offset,
                           index_type);
    ++cur_extent;
    return ret;
}

