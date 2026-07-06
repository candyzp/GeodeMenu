#pragma once

#include "Range.hpp"

namespace qolmod
{
    struct Ranges
    {
        std::vector<Range> ranges = {};

        bool getEnable(double value, bool def, bool* inAnyRange = nullptr);

        void addRange(double min, double max, bool enable);
        void addRange(qolmod::Range range);

        void sort();
        void clear();
        bool isEmpty();

        matjson::Value save();
        void load(const matjson::Value& value);
    };
};