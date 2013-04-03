#ifndef STS_ONLINE_ONLINE_MCMC_MOVE_H
#define STS_ONLINE_ONLINE_MCMC_MOVE_H

#include <smctc.hh>

namespace sts { namespace online {

// Forwards
class TreeParticle;

class OnlineMCMCMove
{
public:
    OnlineMCMCMove();
    virtual ~OnlineMCMCMove() {};

    double acceptanceProbability() const;

    int operator()(long, smc::particle<TreeParticle>&, smc::rng*);
protected:
    virtual int proposeMove(long time, smc::particle<TreeParticle>& particle, smc::rng* rng) = 0;

    /// Number of times the move was attempted
    unsigned int n_attempted;
    /// Number of times the move was accepted
    unsigned int n_accepted;
};

}}

#endif
