#include "lcfit_online_add_sequence_move.h"

#include <memory>
#include <string>
#include <vector>
#include <boost/math/tools/roots.hpp>
#include <boost/numeric/quadrature/adaptive.hpp>

#include "composite_tree_likelihood.h"
#include "guided_online_add_sequence_move.h"
#include "lcfit.h"
#include "lcfit_cpp.h"
#include "tree_particle.h"
#include "tripod_optimizer.h"

namespace sts { namespace online {

class LcfitRejectionSampler {
private:
    smc::rng* rng_;
    bsm_t model_;

    double ml_t_;
    double ml_ll_;

    double t_min_;
    double t_max_;

    double auc_;

public:
    LcfitRejectionSampler(smc::rng* rng, const bsm_t& model) :
        rng_(rng), model_(model)
    {
        ml_t_ = lcfit_bsm_ml_t(&model_);
        ml_ll_ = lcfit_bsm_log_like(ml_t_, &model_);

        std::tie(t_min_, t_max_) = find_bounds();
        auc_ = integrate();

        assert(std::isfinite(t_min_) && t_min_ >= 0.0);
        assert(std::isfinite(t_max_) && t_max_ > t_min_);
    }

    const std::pair<double, double> sample() const {
        double t = 0.0;
        double y = 0.0;

        auto f = [=](double t) -> double { return std::exp(lcfit_bsm_log_like(t, &model_) - ml_ll_); };
        do {
            t = rng_->Uniform(t_min_, t_max_);
            y = rng_->Uniform(0.0, 1.0);
        } while (y > f(t));

        return std::make_pair(t, std::log(f(t) / auc_));
    }

private:
    const std::pair<double, double> find_bounds(double ll_threshold=-10.0) const {
        auto f = [=](double t) -> double { return lcfit_bsm_log_like(t, &model_) - ml_ll_ - ll_threshold; };

        std::pair<double, double> bounds;
        boost::math::tools::eps_tolerance<double> tolerance(30);
        boost::uintmax_t max_iters;

        max_iters = 100;
        bounds = boost::math::tools::toms748_solve(f, 0.0, ml_t_, tolerance, max_iters);
        const double t_min = (bounds.first + bounds.second) / 2.0;
        assert(max_iters <= 100);

        max_iters = 100;
        bounds = boost::math::tools::toms748_solve(f, ml_t_, 10.0, tolerance, max_iters);
        const double t_max = (bounds.first + bounds.second) / 2.0;
        assert(max_iters <= 100);

        return std::make_pair(t_min, t_max);
    }

    double integrate() const {
        auto f = [=](double t) -> double { return std::exp(lcfit_bsm_log_like(t, &model_) - ml_ll_); };

        double result = 0.0;
        double error = 0.0;
        boost::numeric::quadrature::adaptive()(f, t_min_, t_max_, result, error);

        assert(error <= 1e-3);
        return result;
    }
};

LcfitOnlineAddSequenceMove::LcfitOnlineAddSequenceMove(CompositeTreeLikelihood& calculator,
                                                       const std::vector<std::string>& taxaToAdd,
                                                       const std::vector<double>& proposePendantBranchLengths) :
    GuidedOnlineAddSequenceMove(calculator, taxaToAdd, proposePendantBranchLengths)
{ }

AttachmentProposal LcfitOnlineAddSequenceMove::propose(const std::string& leafName, smc::particle<TreeParticle>& particle, smc::rng* rng)
{
    TreeParticle* value = particle.GetValuePointer();
    std::unique_ptr<bpp::TreeTemplate<bpp::Node>>& tree = value->tree;

    // Replace node `n` in the tree with a new node containing as children `n` and `new_node`
    // Attach a new leaf, in the following configuration
    //
    //              father
    //   /          o
    //   |          | d - distal
    //   |          |
    // d | new_node o-------o new_leaf
    //   |          |
    //   |          | distal
    //   \          o
    //              n

    bpp::Node* n = nullptr;
    double edgeLogDensity;
    // branch lengths
    std::tie(n, edgeLogDensity) = chooseEdge(*tree, leafName, rng);
    double mlDistal, mlPendant;
    TripodOptimizer optim = optimizeBranchLengths(n, leafName, mlDistal, mlPendant);

    const double d = n->getDistanceToFather();
    double distal = -1;

    // Handle very small branch lengths - attach with distal BL of 0
    if(d < 1e-8)
        distal = 0;
    else {
        do {
            distal = rng->NormalTruncated(mlDistal, d / 4, 0.0);
        } while(distal < 0 || distal > d);
    }
    assert(!std::isnan(distal));

    const double distalLogDensity = std::log(gsl_ran_gaussian_pdf(distal - mlDistal, d / 4));
    assert(!std::isnan(distalLogDensity));

    //
    // lcfit magic...
    //

    using namespace std::placeholders;
    auto logLike_f = std::bind(&TripodOptimizer::log_like, &optim, distal, _1, false);

    bsm_t model = DEFAULT_INIT;
    lcfit::LCFitResult result = lcfit::fit_bsm_log_likelihood(logLike_f, model, {0.1, 0.15, 0.5});

    LcfitRejectionSampler s(rng, result.model_fit);

    double pendantBranchLength, pendantLogDensity;
    std::tie(pendantBranchLength, pendantLogDensity) = s.sample();

    //const double pendantBranchLength = rng->Exponential(mlPendant);
    //const double pendantLogDensity = std::log(gsl_ran_exponential_pdf(pendantBranchLength, mlPendant));
    assert(!std::isnan(pendantLogDensity));

    return AttachmentProposal { n, edgeLogDensity, distal, distalLogDensity, pendantBranchLength, pendantLogDensity, mlDistal, mlPendant };
}

}} // namespace sts::online
