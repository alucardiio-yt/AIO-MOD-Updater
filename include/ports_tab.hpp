#pragma once

#include <borealis.hpp>
#include <json.hpp>

class PortsTab : public brls::List
{
public:
    PortsTab();

private:
    void createList();
    void displayNotFound();
};
