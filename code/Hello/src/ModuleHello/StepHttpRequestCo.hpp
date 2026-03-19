#pragma once
#include "step/StepCoroutine.hpp"

namespace hello {

class StepHttpRequestCo : public thunder::StepCoroutine {
public:
    StepHttpRequestCo();
    virtual ~StepHttpRequestCo();

    // 协程体
    thunder::CoTask Run() override;
};

}  // namespace hello
