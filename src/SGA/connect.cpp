//-----------------------------------------------
// Copyright 2010 Wellcome Trust Sanger Institute
// Written by Jared Simpson (js18@sanger.ac.uk)
// Released under the GPL
//-----------------------------------------------
//
// connect - Determine the complete sequence of a 
// paired end fragment by finding a walk that
// connects the ends.
//
#include <iostream>
#include <fstream>
#include <sstream>
#include <iterator>
#include "Util.h"
#include "connect.h"
#include "SuffixArray.h"
#include "BWT.h"
#include "SGACommon.h"
#include "OverlapCommon.h"
#include "Timer.h"
#include "BWTAlgorithms.h"
#include "ASQG.h"
#include "gzstream.h"
#include "SequenceProcessFramework.h"
#include "ConnectProcess.h"
#include "SGUtil.h"
#include "gmap.h"
#include "SGSearch.h"

// Struct
// Functions

//
// Getopt
//
#define SUBPROGRAM "connect"
static const char *CONNECT_VERSION_MESSAGE =
SUBPROGRAM " Version " PACKAGE_VERSION "\n"
"Written by Jared Simpson.\n"
"\n"
"Copyright 2010 Wellcome Trust Sanger Institute\n";

static const char *CONNECT_USAGE_MESSAGE =
"Usage: " PACKAGE_NAME " " SUBPROGRAM " [OPTION] ... ASQGFILE GMAPFILE\n"
"Resolve the complete sequence of a paired end fragment by finding a walk through the graph connecting the ends\n"
"The read adjacency information is given in ASQGFILE, which is the direct output from the sga-overlap step.\n"
"It should not contain duplicate reads. The GMAPFILE specifies the vertices to walk between, read pairs\n"
"are assumed to be on consecutive lines.\n"
"\n"
"      --help                           display this help and exit\n"
"      -v, --verbose                    display verbose output\n"
//"      -t, --threads=NUM                use NUM threads to compute the overlaps (default: 1)\n"
"      -m, --max-distance=LEN           maximum expected distance between the PE reads (start to end). This option specifies\n"
"                                       how long the search should proceed for. Default: 250\n"
"      -o, --outfile=FILE               write the connected reads to FILE\n"
"\nReport bugs to " PACKAGE_BUGREPORT "\n\n";

static const char* PROGRAM_IDENT =
PACKAGE_NAME "::" SUBPROGRAM;

namespace opt
{
    static unsigned int verbose;
    static int numThreads = 1;
    static int maxDistance = 250;
    
    static std::string outFile;
    static std::string unconnectedFile = "unconnected.fa";
    static std::string asqgFile;
    static std::string gmapFile;
}

static const char* shortopts = "p:m:e:t:l:s:o:d:v";

enum { OPT_HELP = 1, OPT_VERSION, OPT_METRICS };

static const struct option longopts[] = {
    { "verbose",     no_argument,       NULL, 'v' },
    { "threads",     required_argument, NULL, 't' },
    { "max-distance",required_argument, NULL, 'd' },
    { "outfile",     required_argument, NULL, 'o' },
    { "help",        no_argument,       NULL, OPT_HELP },
    { "version",     no_argument,       NULL, OPT_VERSION },
    { NULL, 0, NULL, 0 }
};

//
// Main
//
int connectMain(int argc, char** argv)
{
    parseConnectOptions(argc, argv);
    Timer* pTimer = new Timer(PROGRAM_IDENT);

    // Read the graph and compute walks
    StringGraph* pGraph = SGUtil::loadASQG(opt::asqgFile, 0, false);
    std::istream* pReader = createReader(opt::gmapFile);
    std::ostream* pWriter = createWriter(opt::outFile);

    GmapRecord record1;
    GmapRecord record2;
    
    int numPairsAttempted = 0;
    int numPairsResolved = 0;

    while(*pReader >> record1)
    {
        bool good = *pReader >> record2;
        assert(good);
    
        if(!record1.isMapped() || !record2.isMapped())
            continue;

        // Ensure the pairing is correct
        assert(getPairID(record1.readID) == record2.readID);

        // Get the vertices for this pair using the mapped IDs
        Vertex* pX = pGraph->getVertex(record1.mappedID);
        Vertex* pY = pGraph->getVertex(record2.mappedID);

        // Skip the pair if either vertex is not found
        if(pX == NULL || pY == NULL)
            continue;

        EdgeDir walkDirection = ED_SENSE;
        if(record1.isRC)
            walkDirection = !walkDirection;

        SGWalkVector walks;
        SGSearch::findWalks(pX, pY, walkDirection, opt::maxDistance, 10000, walks);

        if(walks.size() == 1)
        {
            SeqRecord resolved;
            resolved.id = getPairBasename(record1.readID);
            resolved.seq = walks.front().getString(SGWT_START_TO_END);
            resolved.write(*pWriter);
            numPairsResolved += 1;
        }
        else
        {
            // Write the unconnected reads
            SeqRecord unresolved1;
            unresolved1.id = record1.readID;
            unresolved1.seq = record1.readSeq;
            
            SeqRecord unresolved2;
            unresolved2.id = record2.readID;
            unresolved2.seq = record2.readSeq;

            unresolved1.write(*pWriter);
            unresolved2.write(*pWriter);
        }
        numPairsAttempted += 1;

        if(numPairsAttempted % 50000 == 0)
            printf("[sga connect] Processed %d pairs\n", numPairsAttempted);
    }

    double proc_time_secs = pTimer->getElapsedWallTime();
    printf("connect: Resolved %d out of %d pairs (%lf) in %lfs (%lf pairs/s)\n",
            numPairsResolved, numPairsAttempted, 
            (double)numPairsResolved / numPairsAttempted,
            proc_time_secs,
            numPairsAttempted / proc_time_secs);

    delete pTimer;
    delete pGraph;
    delete pReader;
    delete pWriter;
    return 0;
}

// 
// Handle command line arguments
//
void parseConnectOptions(int argc, char** argv)
{
    std::string algo_str;
    bool die = false;
    for (char c; (c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1;) 
    {
        std::istringstream arg(optarg != NULL ? optarg : "");
        switch (c) 
        {
            case 'o': arg >> opt::outFile; break;
            case 'd': arg >> opt::maxDistance; break;
            case 't': arg >> opt::numThreads; break;
            case '?': die = true; break;
            case 'v': opt::verbose++; break;
            case OPT_HELP:
                std::cout << CONNECT_USAGE_MESSAGE;
                exit(EXIT_SUCCESS);
            case OPT_VERSION:
                std::cout << CONNECT_VERSION_MESSAGE;
                exit(EXIT_SUCCESS);
        }
    }

    if (argc - optind < 2) 
    {
        std::cerr << SUBPROGRAM ": missing arguments\n";
        die = true;
    } 
    else if (argc - optind > 2) 
    {
        std::cerr << SUBPROGRAM ": too many arguments\n";
        die = true;
    }

    if(opt::numThreads <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid number of threads: " << opt::numThreads << "\n";
        die = true;
    }

    if (die) 
    {
        std::cout << "\n" << CONNECT_USAGE_MESSAGE;
        exit(EXIT_FAILURE);
    }

    // Parse the input filenames
    opt::asqgFile = argv[optind++];
    opt::gmapFile = argv[optind++];

    if(opt::outFile.empty())
    {
        std::string prefix = stripFilename(opt::gmapFile);
        opt::outFile = prefix + ".connect.fa";
        opt::unconnectedFile = prefix + ".single.fa";
    }
}
