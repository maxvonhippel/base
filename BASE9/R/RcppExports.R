# This file was generated by Rcpp::compileAttributes
# Generator token: 10BE3573-1514-4C36-9D1C-5A225CD40393

listFilters <- function() {
    .Call('BASE9_listFilters', PACKAGE = 'BASE9')
}

getAGBt_zmass <- function() {
    .Call('BASE9_getAGBt_zmass', PACKAGE = 'BASE9')
}

initBase <- function(modelDir, msModel, wdModel, ifmr) {
    invisible(.Call('BASE9_initBase', PACKAGE = 'BASE9', modelDir, msModel, wdModel, ifmr))
}

setClusterParameters <- function(age, feh, distMod, av, y, carbonicity) {
    invisible(.Call('BASE9_setClusterParameters', PACKAGE = 'BASE9', age, feh, distMod, av, y, carbonicity))
}

changeModels <- function(msModel, wdModel, ifmr) {
    invisible(.Call('BASE9_changeModels', PACKAGE = 'BASE9', msModel, wdModel, ifmr))
}

setIFMRParameters <- function(intercept, slope, quadCoef) {
    invisible(.Call('BASE9_setIFMRParameters', PACKAGE = 'BASE9', intercept, slope, quadCoef))
}

evolve <- function(mass1, mass2) {
    .Call('BASE9_evolve', PACKAGE = 'BASE9', mass1, mass2)
}

