#ifndef DATASERIES_OUTPUTSERIESMODULE_HPP
#define DATASERIES_OUTPUTSERIESMODULE_HPP

#include <DataSeries/DataSeriesModule.hpp>

class OutputSeriesModule : public DataSeriesModule {
public:
    typedef boost::shared_ptr<OutputSeriesModule> OSMPtr;

    ExtentSeries output_series;

    Extent *returnOutputSeries() {
        Extent *ret = output_series.getExtent();
        output_series.clearExtent();
        return ret;
    }

};

#endif
