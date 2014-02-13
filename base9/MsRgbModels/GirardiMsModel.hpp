#ifndef GGIRMAG_H
#define GGIRMAG_H

#include <string>
#include <vector>

#include "../MsRgbModel.hpp"

const int N_GIR_Z         = 8;    /* number of metallicities in Girardi isochrones */
const int N_GIR_AGES      = 50;   /* number of ages in Girardi isochonres */
const int N_GIR_FILTS     = 8;
const int MAX_GIR_ENTRIES = 10000;        /* max Girardi entries for given Z */

class GirardiMsModel : public MsRgbModel
{
  public:
    GirardiMsModel() {;}
    virtual ~GirardiMsModel() {;}

    virtual double deriveAgbTipMass(const std::vector<int>&, double, double, double);
    virtual double wdPrecLogAge(double, double);
    virtual void loadModel(std::string, MsFilter);

  protected:
    virtual int numFilts() const { return N_GIR_FILTS; }

  private:
    double interpInMass (const std::vector<int>&, int, double, int, double *);
};

#endif
