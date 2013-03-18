#include "beagle_tree_likelihood.h"
#include "bpp_shim.h"
#include "util.h"

#include <Bpp/Numeric/Prob/DiscreteDistribution.h>
#include <Bpp/Phyl/Model/SubstitutionModel.h>
#include <Bpp/Seq/Container/SiteContainer.h>

#include <cassert>
#include <stdexcept>
#include <stack>
#include <forward_list>

#include <iostream>

#include <unordered_set>


using sts::likelihood::blit_vector_to_array;
using sts::likelihood::blit_matrix_to_array;
using sts::likelihood::get_partials;

namespace sts { namespace online {


/// \brief Extract a vector of nodes in postorder from \c root
///
/// \param root Tree root
std::vector<const bpp::Node*> postorder(const bpp::Node* root)
{
    std::stack<const bpp::Node*> to_process;
    std::forward_list<const bpp::Node*> result;
    to_process.push(root);
    while(!to_process.empty()) {
        const bpp::Node* n = to_process.top();
        to_process.pop();
        result.push_front(n);
        for(size_t i = 0; i < n->getNumberOfSons(); i++)
            to_process.push(n->getSon(i));
    }
    return std::vector<const bpp::Node*>(result.begin(), result.end());
}

std::vector<const bpp::Node*> preorder(const bpp::Node* root)
{
    std::vector<const bpp::Node*> result;
    std::stack<const bpp::Node*> to_process;
    to_process.push(root);
    while(!to_process.empty()) {
        const bpp::Node* n = to_process.top();
        to_process.pop();
        result.push_back(n);

        // Add sons in reverse order for left-to-right traversal: FILO
        for(int i = n->getNumberOfSons() - 1; i >= 0; i--)
            to_process.push(n->getSon(i));

    }
    return result;
}

std::vector<const bpp::Node*> siblings(const bpp::Node* node)
{
    std::vector<const bpp::Node*> result;
    if(!node->hasFather())
        return result;
    const bpp::Node* f = node->getFather();
    for(size_t i = 0; i < f->getNumberOfSons(); i++) {
        const bpp::Node* n = f->getSon(i);
        if(n != node)
            result.push_back(n);
    }
    return result;
}

inline void beagle_check(int return_code)
{
    if(return_code != BEAGLE_SUCCESS)
        throw std::runtime_error(sts::util::beagle_errstring(return_code));
}

Beagle_tree_likelihood::Beagle_tree_likelihood(const bpp::SiteContainer& sites,
                                               const bpp::SubstitutionModel& model,
                                               const bpp::DiscreteDistribution& rate_dist) :
    beagle_instance(-1),
    n_sites(sites.getNumberOfSites()),
    n_states(model.getNumberOfStates()),
    n_rates(rate_dist.getNumberOfCategories()),
    n_seqs(sites.getNumberOfSequences()),
    n_buffers(4 * n_seqs - 2)  // One buffer for each leaf, one for each internal node, repeated for distal, proximal
{
    assert(n_rates >= 1);

    leaf_buffer.reserve(n_seqs);

    beagle_instance = beagleCreateInstance(
            0,              // Number of tip data elements (input)
            n_buffers,      // Number of partials buffers to create (input)
            0,              // Number of compact state representation buffers to create (input)
            n_states,       // Number of states in the continuous-time Markov chain (input)
            n_sites,        // Number of site patterns to be handled by the instance (input)
            1,              // Number of rate matrix eigen-decomposition buffers to allocate (input)
            n_buffers,      // Number of rate matrix buffers (input)
            n_rates,        // Number of rate categories (input)
            n_buffers + 2,  // Number of scaling buffers - 1 extra buffer for prox, distal
            NULL,           // List of potential resource on which this instance is allowed (input, NULL implies no restriction
            0,              // Length of resourceList list (input)
            BEAGLE_FLAG_VECTOR_SSE | BEAGLE_FLAG_PRECISION_DOUBLE | BEAGLE_FLAG_SCALING_AUTO, // Bit-flags indicating preferred implementation charactertistics, see BeagleFlags (input)
            0,              // Bit-flags indicating required implementation characteristics, see BeagleFlags (input)
            &instance_details);
    if(beagle_instance < 0)
        beagle_check(beagle_instance);

    // Load tips
    for(size_t i = 0; i < n_seqs; i++)
        register_leaf(sites.getSequence(i), model);

    // Weight all sites equally -
    // for online inference, we don't want to compress sites.
    std::vector<double> pattern_weights(sites.getNumberOfSites(), 1.0);
    beagleSetPatternWeights(beagle_instance, pattern_weights.data());
}

Beagle_tree_likelihood::~Beagle_tree_likelihood()
{
    if(beagle_instance >= 0)
        beagleFinalizeInstance(beagle_instance);
}

size_t Beagle_tree_likelihood::register_leaf(const bpp::Sequence& sequence,
                                             const bpp::SubstitutionModel& model)
{
    verify_initialized();
    int buffer = leaf_buffer.size();
    if(leaf_buffer.count(sequence.getName()) > 0)
        throw std::runtime_error("Duplicate sequence name: " + sequence.getName());

    leaf_buffer[sequence.getName()] = buffer;
    const std::vector<double> seq_partials = get_partials(sequence, model, n_rates);
    assert(seq_partials.size() == sequence.size() * n_states * n_rates);
    beagleSetPartials(beagle_instance, buffer, seq_partials.data());
    return buffer;
}

void Beagle_tree_likelihood::load_substitution_model(const bpp::SubstitutionModel& model)
{
    verify_initialized();
    // Eigendecomposition
    const std::unique_ptr<double[]> evec(new double[n_states * n_states]),
                                    ivec(new double[n_states * n_states]),
                                    eval(new double[n_states]);
    blit_matrix_to_array(ivec.get(), model.getRowLeftEigenVectors());     // inverse eigenvectors
    blit_matrix_to_array(evec.get(), model.getColumnRightEigenVectors()); // eigenvectors
    blit_vector_to_array(eval.get(), model.getEigenValues());
    int r;
    r = beagleSetEigenDecomposition(beagle_instance, 0, evec.get(), ivec.get(), eval.get());
    beagle_check(r);

    // State frequencies
    r = beagleSetStateFrequencies(beagle_instance, 0, model.getFrequencies().data());
    beagle_check(r);
}


void Beagle_tree_likelihood::load_rate_distribution(const bpp::DiscreteDistribution& rate_dist)
{
    assert(rate_dist.getNumberOfCategories() == n_rates &&
           "Unexpected rate category count");
    verify_initialized();
    const std::vector<double>& categories = rate_dist.getCategories();
    const std::vector<double>& weights = rate_dist.getProbabilities();

    int r;
    r = beagleSetCategoryRates(beagle_instance, categories.data());
    beagle_check(r);
    r = beagleSetCategoryWeights(beagle_instance, 0, weights.data());
    beagle_check(r);
}

void Beagle_tree_likelihood::calculate_distal_partials(const bpp::TreeTemplate<bpp::Node>& tree,
                                                       std::unordered_map<const bpp::Node*, int>& node_buffer)
{
    verify_initialized();
    const std::vector<const bpp::Node*> postorder_nodes = postorder(tree.getRootNode());
    assert(postorder_nodes.back() == tree.getRootNode());

    // Map from a bpp Node to its associated BEAGLE buffer
    node_buffer.reserve(n_buffers);

    // Buffers 0-(leaf_buffer.size()-1) contain partials for the leaves.
    // First available buffer:
    int buffer = leaf_buffer.size();

    // For tracking BEAGLE operations
    std::vector<BeagleOperation> operations;
    std::vector<int> node_indices;      // probability indices
    std::vector<double> branch_lengths;

    // Traverse nodes in postorder, adding BeagleOperations to update each
    for(const bpp::Node* n : postorder_nodes) {
        if(n->isLeaf()) {
            const std::string& name = n->getName();
            assert(leaf_buffer.count(name) > 0);
            node_buffer[n] = leaf_buffer.at(name);
        } else {
            assert(buffer < n_buffers);
            assert(n->getNumberOfSons() == 2);
            assert(node_buffer.count(n) == 0);
            for(size_t i = 0; i < 2; ++i) {
                assert(node_buffer.count(n->getSon(i)) > 0);
            }
            node_buffer[n] = buffer;
            int child1_buffer = node_buffer[n->getSon(0)],
                child2_buffer = node_buffer[n->getSon(1)];

            // Create a list of partial likelihood update operations.
            // The order is [dest, destScaling, sourceScaling, source1, matrix1, source2, matrix2].
            // Possible TODO: no scaling supported here. Should there be?
            operations.push_back(BeagleOperation(
                                 {buffer,         // Destination buffer
                                  BEAGLE_OP_NONE, // (output) scaling buffer index
                                  BEAGLE_OP_NONE, // (input) scaling buffer index
                                  child1_buffer,  // Index of first child partials buffer
                                  child1_buffer,  // Index of first child transition matrix
                                  child2_buffer,  // Index of second child partials buffer
                                  child2_buffer})); // Index of second child transition matrix
            node_indices.push_back(child1_buffer);
            branch_lengths.push_back(n->getSon(0)->getDistanceToFather());
            node_indices.push_back(child2_buffer);
            branch_lengths.push_back(n->getSon(1)->getDistanceToFather());

            buffer++;
        }
    }

    update_transitions_partials(operations, branch_lengths, node_indices, n_buffers);
    accumulate_scale_factors(operations, n_buffers);
}

void Beagle_tree_likelihood::calculate_proximal_partials(const bpp::TreeTemplate<bpp::Node>& tree,
                                                         const std::unordered_map<const bpp::Node*, int>& distal_node_buffer,
                                                         std::unordered_map<const bpp::Node*, int>& prox_node_buffer)
{
    verify_initialized();

    const std::vector<const bpp::Node*> preorder_nodes = preorder(tree.getRootNode());
    assert(preorder_nodes.front() == tree.getRootNode());

    // Map from a bpp Node to its associated BEAGLE buffer
    prox_node_buffer.reserve(n_buffers);

    // Buffers 0-(distal_node_buffer.size()-1) contain partials for the leaves and distal partials.
    // First available buffer:
    int buffer = 2 * n_seqs - 1;

    // For tracking BEAGLE operations
    std::vector<BeagleOperation> operations;
    std::vector<int> node_indices;      // probability indices
    std::vector<double> branch_lengths;

    // Traverse internal nodes in preorder, adding BeagleOperations to update each
    size_t c = 0;
    for(const bpp::Node* n : preorder_nodes) {
        c++;
        if(n->isLeaf() || n == tree.getRootNode())
            continue;
        assert(n != tree.getRootNode());
        assert(buffer < n_buffers);
        assert(n->getNumberOfSons() == 2);
        // The proximal likelihood for this node should already be calculated.
        assert(distal_node_buffer.find(n) != distal_node_buffer.end());

        int parent_buffer;
        double parent_dist = n->getDistanceToFather();
        // Special handling for the root node:
        // effectively drop the root by combining distance on either side,
        // use distal likelihood buffer for sibling.
        if(n->getFather() == tree.getRootNode()) {
            const std::vector<const bpp::Node*> s = siblings(n);
            assert(s.size() == 1); // Bifurcating only
            const bpp::Node* p = s.at(0);
            assert(distal_node_buffer.find(p) != distal_node_buffer.end());
            parent_buffer = distal_node_buffer.at(p);
            parent_dist += p->getDistanceToFather();
        } else {
            assert(prox_node_buffer.find(n) != prox_node_buffer.end());
            parent_buffer = prox_node_buffer.at(n);
        }
        for(size_t i = 0; i < 2; ++i) {
            const bpp::Node* son = n->getSon(i);
            const bpp::Node* sibling = n->getSon((i + 1) % 2);
            assert(distal_node_buffer.find(son) != distal_node_buffer.end());
            assert(prox_node_buffer.find(son) == prox_node_buffer.end());
            prox_node_buffer[son] = buffer;

            int sibling_buffer = distal_node_buffer.at(sibling);

            operations.push_back(BeagleOperation(
                                 {buffer,            // Destination buffer
                                  BEAGLE_OP_NONE,    // (output) scaling buffer index
                                  BEAGLE_OP_NONE,    // (input) scaling buffer index
                                  parent_buffer,     // Index of first child partials buffer
                                  parent_buffer,     // Index of first child transition matrix
                                  sibling_buffer,    // Index of second child partials buffer
                                  sibling_buffer})); // Index of second child transition matrix
            node_indices.push_back(parent_buffer);
            branch_lengths.push_back(son->getDistanceToFather());
            node_indices.push_back(sibling_buffer);
            branch_lengths.push_back(sibling->getDistanceToFather());

            buffer++;
        }
    }

    const int root_buffer = operations.back().destinationPartials;
    update_transitions_partials(operations, branch_lengths, node_indices, n_buffers + 1);
    // No scale factor accumulations: we'll only use the proximal values for guided proposals
    accumulate_scale_factors(operations, n_buffers + 1);
}

void Beagle_tree_likelihood::update_transitions_partials(const std::vector<BeagleOperation>& operations,
                                                         const std::vector<double>& branch_lengths,
                                                         const std::vector<int>& node_indices,
                                                         const int scaling_buffer)
{
    assert(branch_lengths.size() == node_indices.size());
    assert(branch_lengths.size() == 2 * operations.size());

    int r;

    // Register topology, branch lengths; update transition matrices.
    r = beagleUpdateTransitionMatrices(beagle_instance,        // instance
                                       0,                      // eigenIndex
                                       node_indices.data(),    // probabilityIndices
                                       NULL,                   // firstDerivativeIndices
                                       NULL,                   // secondDerivativeIndices
                                       branch_lengths.data(),  // edgeLengths
                                       node_indices.size());   // count
    beagle_check(r);

    // Update partials for all traversed nodes
    r = beagleUpdatePartials(beagle_instance, operations.data(), operations.size(), scaling_buffer);
    beagle_check(r);
}

void Beagle_tree_likelihood::accumulate_scale_factors(const std::vector<BeagleOperation>& operations,
                                                      const int scale_buffer)
{
    // Calculate marginal log-likelihood scaling for each node
    const std::unique_ptr<int[]> scale_indices(new int[operations.size()]);
    for(size_t i = 0; i < operations.size(); i++) {
        scale_indices[i] = operations[i].destinationPartials;
    }
    int r = beagleAccumulateScaleFactors(beagle_instance, scale_indices.get(), operations.size(), scale_buffer);
    beagle_check(r);
}

double Beagle_tree_likelihood::calculate_log_likelihood(const bpp::TreeTemplate<bpp::Node>& tree)
{
    verify_initialized();

    // Map from a bpp Node to its associated BEAGLE buffer
    std::unordered_map<const bpp::Node*, int> node_buffer;

    calculate_distal_partials(tree, node_buffer);
    std::unordered_map<const bpp::Node*, int> prox_node_buffer;
    calculate_proximal_partials(tree, node_buffer, prox_node_buffer);

    int root_buffer = node_buffer.at(tree.getRootNode());

    // Calculate root log likelihood
    const int category_weight_index = 0;
    const int state_frequency_index = 0;
    const int scaling_indices = n_buffers;
    double log_likelihood;

    // Re-accumulate scale factors for traversed nodes before calculating log-like
    std::unique_ptr<int[]> indices(new int[n_seqs - 1]);
    size_t i = 0;
    for(const bpp::Node* n : postorder(tree.getRootNode()))
        if(!n->isLeaf())
            indices[i++] = node_buffer.at(n);
    beagleAccumulateScaleFactors(beagle_instance, indices.get(), n_seqs - 1, scaling_indices);

    int r = beagleCalculateRootLogLikelihoods(beagle_instance,
                                              &root_buffer,
                                              &category_weight_index,
                                              &state_frequency_index,
                                              &scaling_indices,
                                              1,
                                              &log_likelihood);
    beagle_check(r);


    return log_likelihood;
}

void Beagle_tree_likelihood::verify_initialized() const
{
    if(beagle_instance < 0)
        throw std::runtime_error("BEAGLE instance not initialized.");
}

}} // Namespace
