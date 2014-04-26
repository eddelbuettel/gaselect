//
//  PLSSimpls.h
//  GenAlgPLS
//
//  Created by David Kepplinger on 15.04.2013.
//  Copyright (c) 2013 __MyCompanyName__. All rights reserved.
//

#ifndef GenAlgPLS_PLSSimpls_h
#define GenAlgPLS_PLSSimpls_h

#include "config.h"

#include <RcppArmadillo.h>
#include "PLS.h"

class PLSSimpls : public PLS {
public:
	PLSSimpls(const arma::mat &X, const arma::vec &Y);
	~PLSSimpls();

	void fit(uint16_t ncomp = 0);
	const arma::mat& getCoefficients() const { return this->coef; }
	const arma::vec& getIntercepts() const { return this->intercepts; };
//	arma::cube getFittedValues() const { return this->fittedValues; }

//	arma::mat getScores() { return this->TT; }
//	arma::mat getYLoadings() { return this->tQ.t(); }
//	arma::mat getProjection() { return this->R; }

	virtual PLS* clone() const;

private:
//	arma::cube fittedValues;
	arma::mat coef; // Cube with coefficients
	arma::vec intercepts; // n x ncomp matrix with intercept terms
	double Ymean; // mean of Y
	arma::rowvec Xmean; // Column means of X

	void centerView();

	/*
	 * Following private variables are only for fitting, but it is faster to not
	 * destroy them after each fit
	 */
	arma::mat R; // X factor weights
	arma::mat V; // Orthogonal loadings
	arma::mat tQ; // Y factor loadings (transposed)
};


#endif
