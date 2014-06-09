#include "composite_tree_likelihood.h"
#include "beagle_tree_likelihood.h"
#include "tripod_optimizer.h"

#include <Bpp/Numeric/Prob/DiscreteDistribution.h>
#include <Bpp/Phyl/Model/SubstitutionModel.h>

using namespace std;
using namespace bpp;

namespace sts { namespace online {

CompositeTreeLikelihood::CompositeTreeLikelihood(std::shared_ptr<BeagleTreeLikelihood> calculator) :
    calculator_(calculator),
    tree_(nullptr)
{}

CompositeTreeLikelihood::CompositeTreeLikelihood(std::shared_ptr<BeagleTreeLikelihood> calculator,
                                                 std::vector<TreeLogLikelihood> additionalLogLikes) :
    calculator_(calculator),
    additionalLogLikes_(additionalLogLikes),
    tree_(nullptr)
{}

double CompositeTreeLikelihood::operator()()
{
    assert(tree_ != nullptr && "Uninitialized tree!");
    double tree_likelihood = calculator_->calculateLogLikelihood();

    for(TreeLogLikelihood& like : additionalLogLikes_)
        tree_likelihood += like(*tree_);

    return tree_likelihood;
}

double CompositeTreeLikelihood::logLikelihood()
{
    return this->operator()();
}

void CompositeTreeLikelihood::initialize(const SubstitutionModel& model,
                                         const DiscreteDistribution& rate_dist,
                                         TreeTemplate<Node>& tree)
{
    calculator_->initialize(model, rate_dist, tree);
    this->tree_ = &tree;
}

void CompositeTreeLikelihood::add(TreeLogLikelihood like)
{
    this->additionalLogLikes_.push_back(like);
}

const std::vector<double> CompositeTreeLikelihood::edgeLogLikelihoods(const std::string& leaf_name, const std::vector<double>& pendant_lengths)
{
    // TODO: incorporate prior

    const int leaf_buffer = calculator_->getLeafBuffer(leaf_name);
    const std::vector<BeagleTreeLikelihood::NodePartials> edge_partials = calculator_->getMidEdgePartials();

    std::vector<double> edge_log_likes(edge_partials.size(), std::numeric_limits<double>::lowest());
    for (const double d : pendant_lengths) {
        for (size_t i = 0; i < edge_partials.size(); ++i) {
            double edge_log_like = calculator_->logDot(edge_partials[i].second, leaf_buffer, d);

            for (const TreeLogLikelihood& like : additionalLogLikes_)
                edge_log_like += like(*tree_);

            edge_log_likes[i] = std::max(edge_log_like, edge_log_likes[i]);
        }
    }

    return edge_log_likes;
}

TripodOptimizer CompositeTreeLikelihood::createOptimizer(const bpp::Node* insertEdge, const std::string& newLeafName)
{
    if(calculator_->freeBufferCount() < 2)
        throw std::runtime_error("Insufficient free BEAGLE buffers: " + std::to_string(calculator_->freeBufferCount()));
    return TripodOptimizer(calculator_, insertEdge, newLeafName, insertEdge->getDistanceToFather());
}

std::vector<std::vector<double>> CompositeTreeLikelihood::calculateAttachmentLikelihoods(const std::string& leafName,
                                                                                         const std::vector<BeagleTreeLikelihood::AttachmentLocation>& attachmentLocations,
                                                                                         const std::vector<double> pendantBranchLengths)
{
    return calculator_->calculateAttachmentLikelihoods(leafName, attachmentLocations, pendantBranchLengths);
}

}} // Namespaces
