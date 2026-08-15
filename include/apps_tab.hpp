#pragma once

#include <borealis.hpp>
#include <json.hpp>

class AppsTab : public brls::List
{
public:
    AppsTab();

private:
    void createList();
    void displayNotFound();
};
