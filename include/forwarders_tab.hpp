#pragma once

#include <borealis.hpp>

class ForwardersTab : public brls::List
{
public:
    ForwardersTab();

private:
    void createList();
    void displayNotFound();
};
