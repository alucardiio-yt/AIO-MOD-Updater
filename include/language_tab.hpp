#pragma once

#include <borealis.hpp>

class LanguageTab : public brls::List
{
public:
    LanguageTab();

    View* getDefaultFocus() override
    {
        return brls::List::getDefaultFocus();
    }
};
