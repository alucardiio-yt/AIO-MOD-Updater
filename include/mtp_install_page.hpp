#pragma once

#include <borealis.hpp>

class MtpInstallPage : public brls::List
{
public:
    MtpInstallPage();

private:
    bool prepareEngine();
    void launchEngine();
    void cleanLegacyMtpState();
};
