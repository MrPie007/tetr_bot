#define TETR_SOLVER_LIBRARY
#include "color.cpp"

struct EvaluationOptions {
    int games=10;
    int maxPieces=2000;
    int lookahead=queuedPiecesToLookAhead;
    uint32_t seed=2;
    string label="default";
    filesystem::path csvPath=executableDirectory()/"solver_eval_results.csv";
    bool writeCsv=true;
    bool quiet=false;
};

struct GameResult {
    uint32_t seed=0;
    size_t pieces=0;
    size_t lines=0;
    size_t lineClears=0;
    size_t tetrises=0;
    size_t holeCreatingMoves=0;
    double holeSamples=0;
    int finalHoles=0;
    int maximumHoles=0;
    int maximumHeight=0;
    bool toppedOut=false;
    TimingSeries searchTime;
    array<size_t,7> pieceCounts{};
};

class SevenBag {
public:
    explicit SevenBag(uint32_t seed):generator(seed) {}

    Piece take()
    {
        ensure(1);
        int index=upcoming.front();
        upcoming.pop_front();
        return all_p[index];
    }

    vector<Piece> preview(size_t count)
    {
        ensure(count);
        vector<Piece> result;
        result.reserve(count);
        auto it=upcoming.begin();
        for(size_t index=0;index<count;index++,it++)
        {
            result.push_back(all_p[*it]);
        }
        return result;
    }

private:
    mt19937 generator;
    deque<int> upcoming;

    void ensure(size_t count)
    {
        while(upcoming.size()<count)
        {
            array<int,7> bag{0,1,2,3,4,5,6};
            shuffle(bag.begin(),bag.end(),generator);
            upcoming.insert(upcoming.end(),bag.begin(),bag.end());
        }
    }
};

void printEvaluationUsage()
{
    cout
        <<"Usage: solver_eval.exe [options]\n"
        <<"  --games N         Number of independent games (default: 10)\n"
        <<"  --max-pieces N    Stop each game after N pieces (default: 2000)\n"
        <<"  --lookahead N     Queued pieces searched, from 0 to 4 (default: 1)\n"
        <<"  --seed N          Base seven-bag seed (default: 2)\n"
        <<"  --label TEXT      Version label stored in the CSV summary\n"
        <<"  --csv PATH        Summary CSV path\n"
        <<"  --no-csv          Do not append a CSV result\n"
        <<"  --quiet           Print only the final summary\n"
        <<"  --help             Show this help\n";
}

uint64_t parseUnsignedArgument(const string& value,const string& option)
{
    size_t parsed=0;
    uint64_t result=0;
    try
    {
        result=stoull(value,&parsed);
    }
    catch(const exception&)
    {
        throw invalid_argument(option+" expects a non-negative integer");
    }
    if(parsed!=value.size())
    {
        throw invalid_argument(option+" expects a non-negative integer");
    }
    return result;
}

EvaluationOptions parseEvaluationOptions(int argc,char* argv[])
{
    EvaluationOptions options;
    for(int index=1;index<argc;index++)
    {
        string argument=argv[index];
        auto requireValue=[&]() -> string {
            if(index+1>=argc)
            {
                throw invalid_argument(argument+" requires a value");
            }
            return argv[++index];
        };

        if(argument=="--help")
        {
            printEvaluationUsage();
            exit(0);
        }
        if(argument=="--games")
        {
            options.games=static_cast<int>(parseUnsignedArgument(requireValue(),argument));
        }
        else if(argument=="--max-pieces")
        {
            options.maxPieces=static_cast<int>(parseUnsignedArgument(requireValue(),argument));
        }
        else if(argument=="--lookahead")
        {
            options.lookahead=static_cast<int>(parseUnsignedArgument(requireValue(),argument));
        }
        else if(argument=="--seed")
        {
            uint64_t seed=parseUnsignedArgument(requireValue(),argument);
            if(seed>numeric_limits<uint32_t>::max())
            {
                throw invalid_argument("--seed must fit in a 32-bit unsigned integer");
            }
            options.seed=static_cast<uint32_t>(seed);
        }
        else if(argument=="--label")
        {
            options.label=requireValue();
        }
        else if(argument=="--csv")
        {
            options.csvPath=requireValue();
        }
        else if(argument=="--no-csv")
        {
            options.writeCsv=false;
        }
        else if(argument=="--quiet")
        {
            options.quiet=true;
        }
        else
        {
            throw invalid_argument("Unknown option: "+argument);
        }
    }

    if(options.games<=0) throw invalid_argument("--games must be at least 1");
    if(options.maxPieces<=0) throw invalid_argument("--max-pieces must be at least 1");
    if(options.lookahead<0 || options.lookahead>=queueLength)
    {
        throw invalid_argument("--lookahead must be between 0 and 4");
    }
    return options;
}

void resetSolverState()
{
    grid.assign(boardRows,vector<int>(boardColumns,0));
    firstInCol.assign(boardColumns,0);
    curQueue.clear();
    retV.clear();
    placements.clear();
    while(!grids.empty()) grids.pop();
    while(!firstInCols.empty()) firstInCols.pop();
}

GameResult evaluateGame(const EvaluationOptions& options,uint32_t gameSeed)
{
    resetSolverState();
    realMaxDepth=options.lookahead;
    maxDepth=options.lookahead;

    GameResult result;
    result.seed=gameSeed;
    SevenBag bag(gameSeed);
    Piece current=bag.take();

    while(result.pieces<static_cast<size_t>(options.maxPieces))
    {
        curQueue=bag.preview(queueLength);
        if(getMoves(current).empty())
        {
            result.toppedOut=true;
            break;
        }

        BoardFeatures before=analyzeGrid();
        auto searchStart=chrono::steady_clock::now();
        array<int,3> choice=getBestPosIterative(current);
        result.searchTime.add(chrono::duration<double,milli>(
            chrono::steady_clock::now()-searchStart
        ).count());

        int landingRow=getLowestRow(choice[0],choice[1],current);
        if(landingRow<0)
        {
            result.toppedOut=true;
            break;
        }
        pushPiece(landingRow,choice[0],choice[1],current);
        int clearedLines=clear_all_grid();
        BoardFeatures after=analyzeGrid();

        result.pieces++;
        result.pieceCounts[current.ind]++;
        result.lines+=clearedLines;
        result.lineClears+=clearedLines>0;
        result.tetrises+=clearedLines==4;
        result.holeCreatingMoves+=after.holes>before.holes;
        result.holeSamples+=after.holes;
        result.finalHoles=after.holes;
        result.maximumHoles=max(result.maximumHoles,after.holes);
        result.maximumHeight=max(result.maximumHeight,after.maximumHeight);

        current=bag.take();
    }
    return result;
}

string evaluationTimestamp()
{
    time_t now=time(nullptr);
    tm localTime{};
    localtime_s(&localTime,&now);
    ostringstream timestamp;
    timestamp<<put_time(&localTime,"%Y-%m-%d %H:%M:%S");
    return timestamp.str();
}

void appendEvaluationCsv(
    const EvaluationOptions& options,
    const vector<GameResult>& games,
    const TimingSeries& survival,
    const TimingSeries& searchTimes,
    double wallTimeMs
)
{
    error_code fileError;
    bool needsHeader=!filesystem::exists(options.csvPath,fileError)
        || filesystem::file_size(options.csvPath,fileError)==0;
    ofstream output(options.csvPath,ios::app);
    if(!output)
    {
        throw runtime_error("Could not append evaluation CSV: "+options.csvPath.string());
    }

    size_t totalPieces=0;
    size_t totalLines=0;
    size_t topOuts=0;
    size_t cappedGames=0;
    size_t holeCreatingMoves=0;
    double holeSamples=0;
    int maximumHoles=0;
    int maximumHeight=0;
    uint32_t shortestGameSeed=games.front().seed;
    size_t shortestGameLength=games.front().pieces;
    for(const GameResult& game:games)
    {
        totalPieces+=game.pieces;
        totalLines+=game.lines;
        topOuts+=game.toppedOut;
        cappedGames+=!game.toppedOut;
        holeCreatingMoves+=game.holeCreatingMoves;
        holeSamples+=game.holeSamples;
        maximumHoles=max(maximumHoles,game.maximumHoles);
        maximumHeight=max(maximumHeight,game.maximumHeight);
        if(game.pieces<shortestGameLength)
        {
            shortestGameLength=game.pieces;
            shortestGameSeed=game.seed;
        }
    }

    if(needsHeader)
    {
        output
            <<"timestamp,label,seed,games,max_pieces,lookahead,total_pieces,"
            <<"top_outs,capped_games,avg_survival,median_survival,min_survival,"
            <<"max_survival,shortest_game_seed,total_lines,lines_per_piece,"
            <<"hole_creating_rate,avg_holes,max_holes,max_height,avg_search_ms,"
            <<"p95_search_ms,max_search_ms,solver_pps,wall_time_ms\n";
    }
    output<<BenchmarkStats::csvEscape(evaluationTimestamp())<<','
          <<BenchmarkStats::csvEscape(options.label)<<','
          <<options.seed<<','<<options.games<<','<<options.maxPieces<<','
          <<options.lookahead<<','<<totalPieces<<','<<topOuts<<','<<cappedGames<<','
          <<survival.average()<<','<<survival.percentile(0.50)<<','
          <<survival.minimum()<<','<<survival.maximum()<<','<<shortestGameSeed<<','
          <<totalLines<<','<<(totalPieces?static_cast<double>(totalLines)/totalPieces:0.0)<<','
          <<(totalPieces?100.0*holeCreatingMoves/totalPieces:0.0)<<','
          <<(totalPieces?holeSamples/totalPieces:0.0)<<','
          <<maximumHoles<<','<<maximumHeight<<','<<searchTimes.average()<<','
          <<searchTimes.percentile(0.95)<<','<<searchTimes.maximum()<<','
          <<(searchTimes.total()>0?1000.0*totalPieces/searchTimes.total():0.0)<<','
          <<wallTimeMs<<'\n';
}

void printEvaluationSummary(
    const EvaluationOptions& options,
    const vector<GameResult>& games,
    const TimingSeries& survival,
    const TimingSeries& searchTimes,
    double wallTimeMs
)
{
    size_t totalPieces=0;
    size_t totalLines=0;
    size_t totalLineClears=0;
    size_t totalTetrises=0;
    size_t topOuts=0;
    size_t holeCreatingMoves=0;
    double holeSamples=0;
    int maximumHoles=0;
    int maximumHeight=0;
    array<size_t,7> pieceCounts{};
    for(const GameResult& game:games)
    {
        totalPieces+=game.pieces;
        totalLines+=game.lines;
        totalLineClears+=game.lineClears;
        totalTetrises+=game.tetrises;
        topOuts+=game.toppedOut;
        holeCreatingMoves+=game.holeCreatingMoves;
        holeSamples+=game.holeSamples;
        maximumHoles=max(maximumHoles,game.maximumHoles);
        maximumHeight=max(maximumHeight,game.maximumHeight);
        for(size_t index=0;index<pieceCounts.size();index++)
        {
            pieceCounts[index]+=game.pieceCounts[index];
        }
    }

    cout<<"\n================ SOLVER EVALUATION ================\n";
    cout<<fixed<<setprecision(3);
    cout<<"Label: "<<options.label<<'\n';
    cout<<"Base seed: "<<options.seed<<'\n';
    cout<<"Games: "<<games.size()<<" ("<<topOuts<<" top-outs, "
        <<games.size()-topOuts<<" reached the piece cap)\n";
    cout<<"Lookahead: "<<options.lookahead<<" queued piece(s)\n";
    cout<<"Total pieces: "<<totalPieces<<'\n';
    cout<<"Survival: average="<<survival.average()
        <<"  median="<<survival.percentile(0.50)
        <<"  min="<<survival.minimum()
        <<"  max="<<survival.maximum()<<'\n';
    cout<<"Lines: "<<totalLines
        <<"  lines/piece="<<(totalPieces?static_cast<double>(totalLines)/totalPieces:0.0)
        <<"  clearing moves="<<totalLineClears
        <<"  tetrises="<<totalTetrises<<'\n';
    cout<<"Holes: creating moves="<<holeCreatingMoves
        <<" ("<<(totalPieces?100.0*holeCreatingMoves/totalPieces:0.0)<<"%)"
        <<"  average cells="<<(totalPieces?holeSamples/totalPieces:0.0)
        <<"  maximum="<<maximumHoles<<'\n';
    cout<<"Maximum stack height: "<<maximumHeight<<'\n';
    cout<<"Search: average="<<searchTimes.average()
        <<" ms  median="<<searchTimes.percentile(0.50)
        <<" ms  p95="<<searchTimes.percentile(0.95)
        <<" ms  max="<<searchTimes.maximum()<<" ms\n";
    cout<<"Solver-only throughput: "
        <<(searchTimes.total()>0?1000.0*totalPieces/searchTimes.total():0.0)
        <<" pieces/s\n";
    cout<<"Evaluation wall time: "<<wallTimeMs<<" ms\n";
    cout<<"Piece distribution:";
    for(size_t index=0;index<pieceCounts.size();index++)
    {
        cout<<' '<<all_pc[index]<<'='<<pieceCounts[index];
    }
    cout<<"\n===================================================\n";
}

int main(int argc,char* argv[])
{
    ios_base::sync_with_stdio(false);
    cin.tie(nullptr);
    try
    {
        EvaluationOptions options=parseEvaluationOptions(argc,argv);
        realMaxDepth=options.lookahead;
        maxDepth=options.lookahead;

        cout<<"Evaluating the production solver with deterministic seven-bag queues...\n";
        auto evaluationStart=chrono::steady_clock::now();
        vector<GameResult> games;
        games.reserve(options.games);
        TimingSeries survival;
        TimingSeries searchTimes;

        for(int gameIndex=0;gameIndex<options.games;gameIndex++)
        {
            uint32_t gameSeed=options.seed
                +static_cast<uint32_t>(gameIndex)*0x9E3779B9u;
            GameResult result=evaluateGame(options,gameSeed);
            survival.add(static_cast<double>(result.pieces));
            searchTimes.samples.insert(
                searchTimes.samples.end(),
                result.searchTime.samples.begin(),
                result.searchTime.samples.end()
            );
            games.push_back(result);

            if(!options.quiet)
            {
                const GameResult& game=games.back();
                cout<<"Game "<<gameIndex+1<<'/'<<options.games
                    <<"  seed="<<game.seed
                    <<"  pieces="<<game.pieces
                    <<"  lines="<<game.lines
                    <<"  max height="<<game.maximumHeight
                    <<"  max holes="<<game.maximumHoles
                    <<"  status="<<(game.toppedOut?"top-out":"piece cap")
                    <<'\n';
            }
        }

        double wallTimeMs=chrono::duration<double,milli>(
            chrono::steady_clock::now()-evaluationStart
        ).count();
        printEvaluationSummary(options,games,survival,searchTimes,wallTimeMs);
        if(options.writeCsv)
        {
            appendEvaluationCsv(options,games,survival,searchTimes,wallTimeMs);
            cout<<"Summary appended to "<<options.csvPath.string()<<'\n';
        }
        return 0;
    }
    catch(const exception& error)
    {
        cerr<<"Evaluation error: "<<error.what()<<'\n';
        return 1;
    }
}
