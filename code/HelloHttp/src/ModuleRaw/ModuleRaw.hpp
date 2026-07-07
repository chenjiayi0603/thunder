#pragma once
#include "cmd/Module.hpp"
#include "labor/Labor.hpp"

class ModuleRaw : public net::Module
{
public:
    static net::Module* create() { return new ModuleRaw(); }
    bool AnyMessage(const net::tagMsgShell&, const HttpMsg&) override;
};
