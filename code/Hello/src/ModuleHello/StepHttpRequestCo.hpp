#pragma once
#include "step/StepCoroutine.hpp"

namespace hello
{

class StepHttpRequestCo : public net::StepCoroutine
{
public:
    StepHttpRequestCo();
    virtual ~StepHttpRequestCo();

    net::CoTask Run() override;
};

} // namespace hello
