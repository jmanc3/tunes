#pragma once
#include <string>

struct RGBA  {
    double r = 0;
    double g = 0;
    double b = 0;
    double a = 0;
    
    RGBA() {};

    RGBA(std::string hex) {
        
    }
    
    RGBA(double r, double g, double b, double a) {
        this->r = r;
        this->g = g;
        this->b = b;
        this->a = a;
    }
};

