#ifndef STELLARMODEL_H
#define STELLARMODEL_H

#include <stdexcept>
#include <string>

#include "constants.hpp"

class StellarModel
{
  public:
    virtual void loadModel (std::string path) = 0;
};


class InvalidModel : virtual public StellarModel
{
    virtual void loadModel(std::string) {;}
};


class InvalidModelError : public std::domain_error
{
//    using std::domain_error::domain_error;

  public:
    explicit InvalidModelError (const std::string& what_arg)
        : std::domain_error(what_arg) {}

    explicit InvalidModelError (const char* what_arg)
        : std::domain_error(what_arg) {}
};
#endif
