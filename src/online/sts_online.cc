#include <Bpp/Phyl/Io/NexusIoTree.h>
#include <Bpp/Phyl/Model/Nucleotide/JCnuc.h>
#include <Bpp/Phyl/Model/RateDistribution/ConstantRateDistribution.h>
#include <Bpp/Phyl/TreeTemplateTools.h>
#include <Bpp/Seq/Alphabet/DNA.h>

#include <gsl/gsl_errno.h>
#include <gsl/gsl_randist.h>

#include "tclap/CmdLine.h"

#include "smctc.hh"
#include "json/json.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "sts_config.h"
#include "branch_length_prior.h"
#include "beagle_tree_likelihood.h"
#include "composite_tree_likelihood.h"
#include "uniform_online_add_sequence_move.h"
#include "uniform_length_online_add_sequence_move.h"
#include "gsl.h"
#include "guided_online_add_sequence_move.h"
#include "lcfit_online_add_sequence_move.h"
#include "online_smc_init.h"
#include "multiplier_mcmc_move.h"
#include "node_slider_mcmc_move.h"
#include "multiplier_smc_move.h"
#include "node_slider_smc_move.h"
#include "tree_particle.h"
#include "weighted_selector.h"
#include "util.h"


namespace cl = TCLAP;
using namespace std;
typedef bpp::TreeTemplate<bpp::Node> Tree;

using namespace sts::online;

const bpp::DNA DNA;

/// Partition an alignment into reference and query sequences
/// \param allSequences Site container with all sequences
/// \param taxaInTree Names of reference sequences
/// \param ref *out* Reference alignment
/// \param query *out* Query alignment.
void partitionAlignment(const bpp::SiteContainer& allSequences,
                        const vector<string> taxaInTree,
                        bpp::SiteContainer& ref,
                        bpp::SiteContainer& query)
{
    unordered_set<string> ref_taxa(begin(taxaInTree), end(taxaInTree));
    for(size_t i = 0; i < allSequences.getNumberOfSequences(); i++) {
        const bpp::Sequence& sequence = allSequences.getSequence(i);
        if(ref_taxa.count(sequence.getName()))
            ref.addSequence(sequence, false);
        else
            query.addSequence(sequence, false);
    }
}

vector<unique_ptr<Tree>> readTrees(bpp::IMultiTree& reader, std::string path)
{
    vector<bpp::Tree*> unmanagedTrees;
    reader.read(path, unmanagedTrees);
    vector<unique_ptr<Tree>> result;
    result.reserve(unmanagedTrees.size());
    for(bpp::Tree* t : unmanagedTrees) {
        Tree *tt = new Tree(*t);
        delete(t);

        // Trees must be bifurcating for use with BEAGLE.
        // Root by making the first leaf an outgroup
        tt->newOutGroup(tt->getLeaves()[0]);
        tt->resetNodesId();
        assert(!tt->isMultifurcating());
        assert(tt->isRooted());
        result.emplace_back(tt);
    }
    return result;
}

template<typename T>
class RangeConstraint : public TCLAP::Constraint<T>
{
public:
    RangeConstraint(T minValue, T maxValue, bool inclusive=true) :
        minValue(minValue),
        maxValue(maxValue),
        inclusive(inclusive)
    {};

    std::string shortID() const
    {
        const char start = inclusive ? '[' : '(',
                   end = inclusive ? ']' : ')';
        return start + std::to_string(minValue) + ',' + std::to_string(maxValue) + end;
    };
    std::string description() const { return shortID(); };

    bool check(const T& val) const
    {
        if(inclusive)
            return val >= minValue && val <= maxValue;
        else
            return val > minValue && val < maxValue;
    }
private:
    T minValue, maxValue;
    bool inclusive;
};

std::unique_ptr<OnlineAddSequenceMove> getSequenceMove(CompositeTreeLikelihood& treeLike,
                                                       const std::string& name,
                                                       const double expPriorMean,
                                                       const std::vector<std::string>& queryNames,
                                                       const std::vector<double>& pendantBranchLengths,
                                                       const size_t subdivideTop = 0,
                                                       const double maxLength = std::numeric_limits<double>::max())
{
    if(name == "uniform-length" || name == "uniform-edge") {
        auto branchLengthProposer = [expPriorMean](smc::rng* rng) -> std::pair<double, double> {
            const double v = rng->Exponential(expPriorMean);
            const double logDensity = std::log(gsl_ran_exponential_pdf(v, expPriorMean));
            return {v, logDensity};
        };
        if(name == "uniform-length") {
            return std::unique_ptr<OnlineAddSequenceMove>(new UniformLengthOnlineAddSequenceMove(treeLike, queryNames, branchLengthProposer));
        } else {
            return std::unique_ptr<OnlineAddSequenceMove>(new UniformOnlineAddSequenceMove(treeLike, queryNames, branchLengthProposer));
        }
    } else if(name == "guided") {
        return std::unique_ptr<OnlineAddSequenceMove>(new GuidedOnlineAddSequenceMove(treeLike, queryNames, pendantBranchLengths, maxLength, subdivideTop));
    } else if(name == "lcfit") {
        return std::unique_ptr<OnlineAddSequenceMove>(new LcfitOnlineAddSequenceMove(treeLike, queryNames, pendantBranchLengths, maxLength, subdivideTop, expPriorMean));
    }
    throw std::runtime_error("Unknown sequence addition method: " + name);
}

int main(int argc, char **argv)
{
    cl::CmdLine cmd("Run STS starting from an extant posterior", ' ',
                    sts::STS_VERSION);
    cl::ValueArg<int> burnin("b", "burnin-count", "Number of trees to discard as burnin", false, 0, "#", cmd);

    RangeConstraint<double> resample_range(0.0, 1.0, false);
    cl::ValueArg<double> resample_threshold("", "resample-threshold", "Resample when the ESS falls below T * n_particles",
                                            false, 0.99, &resample_range, cmd);
    cl::ValueArg<int> particleFactor("p", "particle-factor", "Multiple of number of trees to determine particle count",
                                      false, 1, "#", cmd);
    cl::ValueArg<int> mcmcCount("m", "mcmc-moves", "Number of MCMC moves per-particle",
                                 false, 0, "#", cmd);
    cl::ValueArg<int> treeSmcCount("", "tree-moves",
                                   "Number of additional tree-altering SMC moves per added sequence",
                                   false, 0, "#", cmd);
    cl::ValueArg<string> particleGraphPath("g", "particle-graph",
                                           "Path to write particle graph in graphviz format",
                                           false, "", "path", cmd);
    cl::ValueArg<double> blPriorExpMean("", "edge-prior-exp-mean", "Mean of exponential prior on edges",
                                           false, 0.1, "float", cmd);

    cl::SwitchArg noGuidedMoves("", "no-guided-moves", "Do *not* use guided attachment proposals", cmd, true);
    std::vector<std::string> methodNames { "uniform-edge", "uniform-length", "guided", "lcfit" };
    cl::ValuesConstraint<std::string> allowedProposalMethods(methodNames);
    cl::ValueArg<std::string> proposalMethod("", "proposal-method", "Proposal mechanism to use", false,
                                             "guided", &allowedProposalMethods, cmd);
    cl::ValueArg<double> maxLength("", "max-length", "When discretizing the tree for guided moves, "
                                   "divide edges into lengths no greater than <length>",
                                   false, std::numeric_limits<double>::max(), "length", cmd);
    cl::ValueArg<size_t> subdivideTop("", "divide-top", "Subdivide the top <N> edges to bits of no longer than max-length.",
                                   false, 0, "N", cmd);
    cl::SwitchArg fribbleResampling("", "fribble", "Use fribblebits resampling method", cmd, false);
    cl::MultiArg<double> pendantBranchLengths("", "pendant-bl", "Guided move: attempt attachment with pendant bl X", false, "X", cmd);

    cl::UnlabeledValueArg<string> alignmentPath(
        "alignment", "Input fasta alignment.", true, "", "fasta", cmd);
    cl::UnlabeledValueArg<string> treePosterior(
        "posterior_trees", "Posterior tree file in NEXUS format",
        true, "", "trees.nex", cmd);

    cl::UnlabeledValueArg<string> jsonOutputPath("json_path", "JSON output path", true, "", "path", cmd);

    //cl::UnlabeledValueArg<string> param_posterior(
        //"posterior_params", "Posterior parameter file, tab delimited",
        //true, "", "params", cmd);

    try {
        cmd.parse(argc, argv);
    } catch(TCLAP::ArgException &e) {
        cerr << "error: " << e.error() << " for arg " << e.argId() << endl;
        return 1;
    }

    // Register a GSL error handler that throws exceptions instead of aborting.
    gsl_set_error_handler(&sts_gsl_error_handler);

    // Get alignment

    // Read trees
    bpp::NexusIOTree treeReader;
    vector<unique_ptr<Tree>> trees;
    try {
        trees = readTrees(treeReader, treePosterior.getValue());
    } catch(bpp::Exception &e) {
        cerr << "error reading " << treePosterior.getValue() << ": " << e.what() << endl;
        return 1;
    }
    // Discard burnin
    if(burnin.getValue() > 0) {
        if(burnin.getValue() >= trees.size()) {
            cerr << "Burnin (" << burnin.getValue() << ") exceeds number of trees (" << trees.size() << ")\n";
            return 1;
        }
        trees.erase(trees.begin(), trees.begin() + burnin.getValue());
    }
    clog << "read " << trees.size() << " trees" << endl;

    ifstream alignment_fp(alignmentPath.getValue());
    unique_ptr<bpp::SiteContainer> sites(sts::util::read_alignment(alignment_fp, &DNA));
    alignment_fp.close();
    bpp::VectorSiteContainer ref(&DNA), query(&DNA);
    partitionAlignment(*sites, trees[0]->getLeavesNames(), ref, query);
    cerr << ref.getNumberOfSequences() << " reference sequences" << endl;
    cerr << query.getNumberOfSequences() << " query sequences" << endl;

    if(query.getNumberOfSequences() == 0)
        throw std::runtime_error("No query sequences!");

    // TODO: allow model specification
    bpp::JCnuc model(&DNA);
    // TODO: Allow rate distribution specification
    bpp::ConstantRateDistribution rate_dist;
    //bpp::GammaDiscreteDistribution rate_dist(4, 0.358);

    // TODO: Other distributions
    const double expPriorMean = blPriorExpMean.getValue();
    std::function<double(double)> exponentialPrior = [expPriorMean](const double d) {
        return std::log(gsl_ran_exponential_pdf(d, expPriorMean));
    };

    vector<TreeParticle> particles;
    particles.reserve(trees.size());
    for(unique_ptr<Tree>& tree : trees) {
        particles.emplace_back(std::unique_ptr<bpp::SubstitutionModel>(model.clone()),
                               std::unique_ptr<bpp::TreeTemplate<bpp::Node>>(tree.release()),
                               std::unique_ptr<bpp::DiscreteDistribution>(rate_dist.clone()),
                               &ref);
    }

    std::shared_ptr<BeagleTreeLikelihood> beagleLike =
        make_shared<BeagleTreeLikelihood>(*sites, model, rate_dist);
    CompositeTreeLikelihood treeLike(beagleLike);
    treeLike.add(BranchLengthPrior(exponentialPrior));

    const int treeMoveCount = treeSmcCount.getValue();
    // move selection
    std::vector<smc::moveset<TreeParticle>::move_fn> smcMoves;

    std::vector<double> pbl = pendantBranchLengths.getValue();
    if(pbl.empty())
        pbl = {0.0, 0.5};
    std::unique_ptr<OnlineAddSequenceMove> onlineAddSequenceMove =
        getSequenceMove(treeLike,
                        proposalMethod.getValue(),
                        expPriorMean,
                        query.getSequencesNames(),
                        pbl,
                        subdivideTop.getValue(),
                        maxLength.getValue());

    {
        using namespace std::placeholders;
        auto wrapper = std::bind(&OnlineAddSequenceMove::operator(), std::ref(*onlineAddSequenceMove), _1, _2, _3);
        smcMoves.push_back(wrapper);
    }

    WeightedSelector<size_t> additionalSMCMoves;
    if(treeMoveCount) {
        smcMoves.push_back(MultiplierSMCMove(treeLike));
        smcMoves.push_back(NodeSliderSMCMove(treeLike));

        // Twice as many multipliers
        additionalSMCMoves.push_back(1, 20);
        additionalSMCMoves.push_back(2, 5);
    }

    std::function<long(long, const smc::particle<TreeParticle>&, smc::rng*)> moveSelector =
        [treeMoveCount,&query,&smcMoves,&additionalSMCMoves](long time, const smc::particle<TreeParticle>&, smc::rng* rng) -> long {
       const size_t blockSize = 1 + treeMoveCount;

       // Add a sequence, followed by treeMoveCount randomly selected moves
       const bool addSequenceStep = (time - 1) % blockSize == 0;
       if(addSequenceStep)
           return 0;
       return additionalSMCMoves.choice();
    };

    // SMC
    OnlineSMCInit particleInitializer(particles);

    smc::sampler<TreeParticle> sampler(particleFactor.getValue() * trees.size(), SMC_HISTORY_NONE);
    smc::mcmc_moves<TreeParticle> mcmcMoves;
    mcmcMoves.AddMove(MultiplierMCMCMove(treeLike), 4.0);
    mcmcMoves.AddMove(NodeSliderMCMCMove(treeLike), 1.0);
    smc::moveset<TreeParticle> moveSet(particleInitializer, moveSelector, smcMoves, mcmcMoves);
    moveSet.SetNumberOfMCMCMoves(mcmcCount.getValue());

    // Output
    Json::Value jsonRoot;
    Json::Value& jsonTrees = jsonRoot["trees"];
    Json::Value& jsonIters = jsonRoot["generations"];
    if(jsonOutputPath.isSet()) {
        Json::Value& v = jsonRoot["run"];
        v["nQuerySeqs"] = static_cast<unsigned int>(query.getNumberOfSequences());
        v["nParticles"] = static_cast<unsigned int>(sampler.GetNumber());
        for(size_t i = 0; i < argc; i++)
            v["args"][i] = argv[i];
        v["version"] = sts::STS_VERSION;
    }

    sampler.SetResampleParams(SMC_RESAMPLE_STRATIFIED, resample_threshold.getValue());
    sampler.SetMoveSet(moveSet);
    sampler.Initialise();
    const size_t nIters = (1 + treeMoveCount) * query.getNumberOfSequences();
    vector<string> sequenceNames = query.getSequencesNames();

    smc::DatabaseHistory database_history;

    for(size_t n = 0; n < nIters; n++) {
        double ess = 0.0;

        if (fribbleResampling.getValue()) {
            ess = sampler.IterateEssVariable(&database_history);
        } else {
            ess = sampler.IterateEss();
        }

        cerr << "Iter " << n << ": ESS=" << ess << " sequence=" << sequenceNames[n / (1 + treeMoveCount)] << endl;
        if(jsonOutputPath.isSet()) {
            Json::Value& v = jsonIters[n];
            v["T"] = static_cast<unsigned int>(n + 1);
            v["ess"] = ess;
            v["sequence"] = sequenceNames[n / (1 + treeMoveCount)];
            v["totalUpdatePartialsCalls"] = static_cast<unsigned int>(BeagleTreeLikelihood::totalBeagleUpdateTransitionsCalls());
            if (fribbleResampling.getValue()) {
                Json::Value ess_array;
                for (size_t i = 0; i < database_history.ess.size(); ++i)
                    ess_array.append(database_history.ess[i]);
                v["essHistory"] = ess_array;
            }
        }
    }

    double maxLogLike = -std::numeric_limits<double>::max();
    for(size_t i = 0; i < sampler.GetNumber(); i++) {
        const TreeParticle& p = sampler.GetParticleValue(i);
        treeLike.initialize(*p.model, *p.rateDist, *p.tree);
        const double logLike = beagleLike->calculateLogLikelihood();
        maxLogLike = std::max(logLike, maxLogLike);
        string s = bpp::TreeTemplateTools::treeToParenthesis(*p.tree);
        if(jsonOutputPath.isSet()) {
            Json::Value& v = jsonTrees[jsonTrees.size()];
            v["treeLogLikelihood"] = logLike;
            v["totalLikelihood"] = treeLike();
            v["newickString"] = s;
            v["logWeight"] = sampler.GetParticleLogWeight(i);
            v["treeLength"] = p.tree->getTotalLength();
        }
    }

    std::vector<ProposalRecord> proposalRecords = onlineAddSequenceMove->getProposalRecords();
    Json::Value& jsonProposals = jsonRoot["proposals"];
    for (size_t i = 0; i < proposalRecords.size(); ++i) {
        const auto& pr = proposalRecords[i];
        Json::Value& v = jsonProposals[i];
        v["T"] = static_cast<unsigned int>(pr.T);
        v["originalLogLike"] = pr.originalLogLike;
        v["newLogLike"] = pr.newLogLike;
        v["originalLogWeight"] = pr.originalLogWeight;
        v["newLogWeight"] = pr.newLogWeight;
        v["distalBranchLength"] = pr.proposal.distalBranchLength;
        v["distalLogProposalDensity"] = pr.proposal.distalLogProposalDensity;
        v["pendantBranchLength"] = pr.proposal.pendantBranchLength;
        v["pendantLogProposalDensity"] = pr.proposal.pendantLogProposalDensity;
        v["edgeLogProposalDensity"] = pr.proposal.edgeLogProposalDensity;
        v["logProposalDensity"] = pr.proposal.logProposalDensity();
        v["mlDistalBranchLength"] = pr.proposal.mlDistalBranchLength;
        v["mlPendantBranchLength"] = pr.proposal.mlPendantBranchLength;

        v["proposalMethodName"] = pr.proposal.proposalMethodName;
    }

    if(jsonOutputPath.isSet()) {
        ofstream jsonOutput(jsonOutputPath.getValue());
        Json::StyledWriter writer;
        jsonOutput << writer.write(jsonRoot);
    }

    if(particleGraphPath.isSet()) {
        ofstream gOut(particleGraphPath.getValue());
        sampler.StreamParticleGraph(gOut);
    }

    clog << "Maximum LL: " << maxLogLike << '\n';
}
