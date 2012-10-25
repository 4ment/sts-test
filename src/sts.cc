#include "sts/likelihood.hpp"
#include "sts/moves.hpp"
#include "sts/particle.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <fstream>
#include <stack>
#include <memory>
#include <unordered_map>
#include <Bpp/Phyl/Model/GTR.h>
#include <Bpp/Phyl/Model/HKY85.h>
#include <Bpp/Phyl/Model/JCnuc.h>
#include <Bpp/Phyl/Model/JTT92.h>
#include <Bpp/Phyl/Model/TN93.h>
#include <Bpp/Phyl/Model/WAG01.h>
#include <Bpp/Phyl/PatternTools.h>
#include <Bpp/Seq/Alphabet/DNA.h>
#include <Bpp/Seq/Alphabet/ProteicAlphabet.h>
#include <Bpp/Seq/Container/SequenceContainer.h>
#include <Bpp/Seq/Container/SiteContainer.h>
#include <Bpp/Seq/Container/SiteContainerTools.h>
#include <Bpp/Seq/Container/VectorSiteContainer.h>
#include <Bpp/Seq/Io/IoSequenceFactory.h>
#include <Bpp/Seq/Io/ISequence.h>
#include "tclap/CmdLine.h"
#include <json/json.h>

#define _STRINGIFY(s) #s
#define STRINGIFY(s) _STRINGIFY(s)

using namespace std;
using namespace sts::likelihood;
using namespace sts::moves;
using namespace sts::particle;

const bpp::DNA DNA;
const bpp::RNA RNA;
const bpp::ProteicAlphabet AA;

bpp::SiteContainer* read_alignment(istream &in, const bpp::Alphabet *alphabet)
{
    // Holy boilerplate - Bio++ won't allow reading FASTA files as alignments
    bpp::IOSequenceFactory fac;
    unique_ptr<bpp::ISequence> reader = unique_ptr<bpp::ISequence>(
                                            fac.createReader(bpp::IOSequenceFactory::FASTA_FORMAT));
    unique_ptr<bpp::SequenceContainer> seqs = unique_ptr<bpp::SequenceContainer>(reader->read(in, alphabet));

    // Have to look up by name
    vector<string> names = seqs->getSequencesNames();
    bpp::SiteContainer *sequences = new bpp::VectorSiteContainer(alphabet);

    for(auto it = names.begin(), end = names.end(); it != end; ++it) {
        sequences->addSequence(seqs->getSequence(*it), true);
    }

    bpp::SiteContainerTools::changeGapsToUnknownCharacters(*sequences);

    return sequences;
}

std::vector<std::string> get_model_names()
{
    std::vector<string> models;
    // Nucleotide
    models.push_back("JCnuc");
    models.push_back("HKY");
    models.push_back("GTR");
    models.push_back("TN93");

    // Protein
    models.push_back("JTT");
    models.push_back("WAG");
    return models;
}

/// Get the alphabet & substitution model associated with a name.

/// Model should match option from model_name_arg
shared_ptr<bpp::SubstitutionModel> model_for_name(const string name)
{
    shared_ptr<bpp::SubstitutionModel> model;
    // Nucleotide models
    if(name == "JCnuc" || name == "HKY" || name == "GTR" || name == "TN93") {
        if(name == "JCnuc") model = make_shared<bpp::JCnuc>(&DNA);
        else if(name == "HKY") model = make_shared<bpp::HKY85>(&DNA);
        else if(name == "GTR") model = make_shared<bpp::GTR>(&DNA);
        else if(name == "TN93") model = make_shared<bpp::TN93>(&DNA);
        else assert(false);
    } else {
        // Protein model
        if(name == "JTT") model = make_shared<bpp::JTT92>(&AA);
        else if(name == "WAG") model = make_shared<bpp::WAG01>(&AA);
        else assert(false);
    }
    return model;
}

static bpp::SiteContainer* unique_sites(const bpp::SiteContainer& sites)
{
    bpp::SiteContainer *compressed = bpp::PatternTools::shrinkSiteSet(sites);

    if(compressed->getNumberOfSites() < sites.getNumberOfSites())
        cerr << "Reduced from "
             << sites.getNumberOfSites()
             << " to " << compressed->getNumberOfSites()
             << " sites"
             << endl;

    return compressed;
}

void serialize_particle_system( smc::sampler<particle>& sampler, ostream& json_out, unordered_map<node, string>& names, int generation, unordered_map< particle, int >& particle_id_map, unordered_map< node, int >& node_id_map)
{
    Json::Value root;
    root["generation"] = generation;
    Json::Value& states = root["particles"];
    Json::Value& particles = root["states"];
    Json::Value& nodes = root["nodes"];
    unordered_set< particle > particles_visited;
    unordered_set< node > nodes_visited;
    
    int nindex = 0;
    int pindex = 0;

    // Add all the leaf nodes if they haven't already been added.
    for(auto n : names){
        if(nodes_visited.count(n.first)) continue;
        nodes_visited.insert(n.first);
        if(node_id_map.count(n.first) == 0) node_id_map[n.first] = node_id_map.size();
        int nid = node_id_map[n.first];
        Json::Value& jnode = nodes[nindex++];
        jnode["id"]=nid;
        jnode["name"]=n.second;
    }        

    // Traverse the particle system and add particles and any internal tree nodes.
    for(int i=0;i<sampler.GetNumber(); i++){
        // determine whether we've seen this particle previously and if not add it
        particle X = sampler.GetParticleValue(i);
        stack<particle> s;
        s.push(X);
        
        while(s.size()>0){
            particle x = s.top();
            s.pop();
            if(x==NULL) continue;
            if(particles_visited.count(x) != 0) continue;
            particles_visited.insert(x);
            particle pred = x->predecessor;
            if(particle_id_map.count(x) == 0) particle_id_map[x] = particle_id_map.size();
            if(particle_id_map.count(pred) == 0) particle_id_map[pred] = particle_id_map.size();
            int pid = particle_id_map[x];
            Json::Value& jpart = particles[pindex++];
            jpart["id"] = pid;
            if(pred!=NULL) jpart["predecessor"] = particle_id_map[pred];
            if(pred!=NULL) s.push(pred);
            
            // traverse the nodes below this particle
            stack<node> ns;
            if(x->node != NULL) ns.push(x->node);
            while(ns.size()>0){
                node n = ns.top();
                ns.pop();
                if(n==NULL) continue;
                if(nodes_visited.count(n) != 0) continue;
                nodes_visited.insert(n);
                if(node_id_map.count(n) == 0) node_id_map[n] = node_id_map.size();
                // Determine whether we've seen this node previously and if not add it
                int nid = node_id_map[n];
                Json::Value& jnode = nodes[nindex++];

                node c1 = n->child1->node;
                node c2 = n->child2->node;
                ns.push(c1);
                ns.push(c2);
                if(node_id_map.count(c1) == 0) node_id_map[c1] = node_id_map.size();
                if(node_id_map.count(c2) == 0) node_id_map[c2] = node_id_map.size();

                jnode["id"]=nid;
                jnode["child1"]=node_id_map[c1];
                jnode["child2"]=node_id_map[c2];
                jnode["length1"]=n->child1->length;
                jnode["length2"]=n->child2->length;
            }

            // Add a node reference to this particle.
            jpart["node"]=node_id_map[x->node];
        }
        states[i] = particle_id_map[X];
    }    

    Json::StyledStreamWriter writer;
    writer.write(json_out, root);
}

int main(int argc, char** argv)
{
    TCLAP::CmdLine cmd("runs sts", ' ', STRINGIFY(STS_VERSION));

    TCLAP::UnlabeledValueArg<string> alignment(
        "alignment", "Input fasta alignment", true, "", "fasta alignment", cmd);
    TCLAP::ValueArg<string> output_path(
        "o", "out", "Where to write the output trees", false, "-", "tsv file", cmd);
    vector<string> all_models = get_model_names();
    TCLAP::ValuesConstraint<string> allowed_models(all_models);
    TCLAP::ValueArg<string> model_name(
        "m", "model-name", "Which substitution model to use", false, "JCnuc", &allowed_models, cmd);
    TCLAP::ValueArg<long> particle_count(
        "p", "particle-count", "Number of particles in the SMC", false, 1000, "#", cmd);
    TCLAP::SwitchArg no_compress("", "no-compress", "Do not compress the alignment to unique sites", cmd, false);

    try {
        cmd.parse(argc, argv);
    } catch(TCLAP::ArgException &e) {
        cerr << "error: " << e.error() << " for arg " << e.argId() << endl;
        return 1;
    }

    const long population_size = particle_count.getValue();
    ifstream in(alignment.getValue().c_str());
    string output_filename = output_path.getValue();
    ostream *output_stream;
    ofstream output_ofstream;
    if (output_filename == "-") {
        output_stream = &cout;
    } else {
        output_ofstream.open(output_filename);
        output_stream = &output_ofstream;
    }

    shared_ptr<bpp::SubstitutionModel> model = model_for_name(model_name.getValue());
    shared_ptr<bpp::SiteContainer> aln = shared_ptr<bpp::SiteContainer>(read_alignment(in, model->getAlphabet()));

    // Compress sites
    if(!no_compress.getValue())
        aln.reset(unique_sites(*aln));
    const int num_iters = aln->getNumberOfSequences();

    // Leaves
    vector<node> leaf_nodes;

    shared_ptr<online_calculator> calc = make_shared<online_calculator>();
    calc->initialize(aln, model);
    leaf_nodes.resize(num_iters);
    unordered_map<node, string> name_map;
    for(int i = 0; i < num_iters; i++) {
        leaf_nodes[i] = make_shared<phylo_node>(calc);
        calc->register_node(leaf_nodes[i]);
        name_map[leaf_nodes[i]] = aln->getSequencesNames()[i];
    }
    forest_likelihood fl(calc, leaf_nodes);
    rooted_merge smc_mv(fl);
    smc_init init(fl);
    uniform_bl_mcmc_move mcmc_mv(fl, 0.1);
    
    ofstream json_out("json.out");
    Json::Value root;
    root["version"]="0.1";
    Json::StyledStreamWriter writer;
    writer.write(json_out, root);
    unordered_map< particle, int > particle_id_map;
    unordered_map< node, int > node_id_map;

    try {

        // Initialise and run the sampler
        smc::sampler<particle> Sampler(population_size, SMC_HISTORY_NONE);
        smc::moveset<particle> Moveset(init, smc_mv, mcmc_mv);

        Sampler.SetResampleParams(SMC_RESAMPLE_STRATIFIED, 0.99);
        Sampler.SetMoveSet(Moveset);
        Sampler.Initialise();

        for(int n = 1 ; n < num_iters ; ++n) {
            Sampler.Iterate();

            double max_ll = -std::numeric_limits<double>::max();
            unordered_set<particle> diversity;
            for(int i = 0; i < population_size; i++) {
                particle X = Sampler.GetParticleValue(i);
                // write the log likelihood
                double ll = fl(X);
                max_ll = max_ll > ll ? max_ll : ll;
                diversity.insert(X);
            }
            cerr << "Iter " << n << " max ll " << max_ll << " diversity " << diversity.size() << endl;            
            serialize_particle_system(Sampler, json_out, name_map, n, particle_id_map, node_id_map);
        }

        for(int i = 0; i < population_size; i++) {
            particle X = Sampler.GetParticleValue(i);
            // write the log likelihood
            *output_stream << fl(X) << "\t";
            // write out the tree under this particle
            write_tree(*output_stream, X->node, name_map);
        }
        
    }

    catch(smc::exception  e) {
        cerr << e;
        return e.lCode;
    }
    return 0;
}
