#include "catch.hpp"

#include <Bpp/Seq/Alphabet/DNA.h>
#include <Bpp/Phyl/Io/Newick.h>
#include <Bpp/Phyl/Model/JCnuc.h>
#include <Bpp/Phyl/Model/HKY85.h>
#include <Bpp/Phyl/Model/SubstitutionModel.h>
#include <Bpp/Phyl/Likelihood/DRHomogeneousTreeLikelihood.h>

#include <Bpp/Numeric/Prob/ConstantDistribution.h>
#include <Bpp/Numeric/Prob/GammaDiscreteDistribution.h>

#include "util.h"
#include "beagle_tree_likelihood.h"

#include <iostream>
#include <fstream>
#include <string>
#include <memory>

namespace sts { namespace test { namespace beagle_tree_likelihood {

const bpp::DNA dna;

void test_known_tree(std::string fasta_path,
                     std::string newick_path,
                     bpp::SubstitutionModel& model,
                     bpp::DiscreteDistribution& rate_dist)
{
    std::ifstream aln_stream(fasta_path);
    bpp::Newick newick_io;
    std::unique_ptr<bpp::Tree> tree(newick_io.read(newick_path));
    std::unique_ptr<bpp::TreeTemplate<bpp::Node>> tt(new bpp::TreeTemplate<bpp::Node>(*tree));
    std::shared_ptr<bpp::SiteContainer> aln(sts::util::read_alignment(aln_stream, &dna));

    // BEAGLE
    sts::likelihood::Beagle_tree_likelihood beagle_calculator(*aln, model, rate_dist);
    beagle_calculator.load_rate_distribution(rate_dist);
    beagle_calculator.load_substitution_model(model);
    const double beagle_ll = beagle_calculator.calculate_log_likelihood(*tt);

    // Bio++
    bpp::DRHomogeneousTreeLikelihood like(*tt, &model, &rate_dist, false, false);
    like.setData(*aln);
    like.initialize();
    const double bpp_ll = -like.getValue();

    CHECK(beagle_ll == Approx(bpp_ll));
}

TEST_CASE("sts/beagle_tree_likelihood/thirty/JC/constant", "thirty.ma, jukes-cantor, constant rates")
{
    bpp::JCnuc model(&dna);
    bpp::ConstantDistribution rates(1.0);
    test_known_tree("data/thirty.ma", "data/thirty.tree", model, rates);
}

TEST_CASE("sts/beagle_tree_likelihood/thirty/JC/gamma4", "thirty.ma, jukes-cantor, gamma4 rates")
{
    bpp::JCnuc model(&dna);
    bpp::GammaDiscreteDistribution rates(4, 0.234);
    test_known_tree("data/thirty.ma", "data/thirty.tree", model, rates);
}

TEST_CASE("sts/beagle_tree_likelihood/thirty/HKY/gamma4", "thirty.ma, HKY, gamma4 rates")
{
    bpp::HKY85 model(&dna, 2.0, 0.4, 0.2, 0.15, 0.25);
    bpp::GammaDiscreteDistribution rates(4, 0.234);
    test_known_tree("data/thirty.ma", "data/thirty.tree", model, rates);
}

}}} // namespaces