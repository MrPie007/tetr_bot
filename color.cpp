#include <windows.h>
#include <bits/stdc++.h>
#include <chrono>
#include <thread>
#include "screen_layout.h"
using namespace std;
/*
info:
expected number of possible moves for each depth:
23.1429
535.592
12395.1
286859
6.63873e+06
1.53639e+08

//estimation of complexity:
each piece has 10 possible positions, and an average of 3~ rotations
30^depth
when depth is 4 (current piece + 3 from queue), then i'd need 810000 (1e6)
it should be much less
actually let me calculate it rq
number of possible moves for each piece:
17 34 34 9 17 17 34 
avg = 23.33~

max is 34^depth (if T,J,L and one of them gets repeated, its not that uncommon to happen..)
34^4 =~ 1e6..
*/
/*
runtime optimization ideas:
represent board as a bitset, do all operations using bitwise operations
Since the depth is constant (not like chess), I can generate all possible moves for the queue, and iterate on them making them all at once

(after implementing that idea:)
the generate all states first approach was slower, I'll just optimize the recursive one as much as I can
(it might be faster if I was able to not clear the grid every time I make a move)
*/
/*
TODO list:
current goal is to reach depth 3 in 0.1~s avg.
~~more~~ fix heuristics for the board eval

non added mechanics:
hold piece ability
side putting
spins
*/
HDC hScreenDC;
HDC hMemoryDC;
BITMAPINFO bmi;
HBITMAP hBitmap;
HANDLE highResolutionTimer;

constexpr int queuedPiecesToLookAhead=1;
constexpr DWORD inputDelayMs=0;
constexpr DWORD screenUpdateDelayMs=1;
constexpr DWORD initialScreenUpdateDelayMs=20;
constexpr DWORD screenUpdateTimeoutMs=100;
constexpr DWORD inputPollingDelayMs=4;
int maxDepth=queuedPiecesToLookAhead;
int realMaxDepth=queuedPiecesToLookAhead;
constexpr int boardColumns=10;
constexpr int boardRows=20;
constexpr int queueLength=5;
constexpr int cellSampleRadius=1;
constexpr int queuePixelStride=2;
ScreenLayout screenLayout;
int x=0;
int y=0;
int width=0;
int height=0;
void* pPixels = nullptr;
unsigned int* pixels;

struct TimingSeries {
    vector<double> samples;

    void add(double milliseconds) {
        samples.push_back(milliseconds);
    }

    double total() const {
        return accumulate(samples.begin(),samples.end(),0.0);
    }

    double average() const {
        return samples.empty()?0.0:total()/samples.size();
    }

    double percentile(double fraction) const {
        if(samples.empty()) return 0.0;
        vector<double> sorted=samples;
        sort(sorted.begin(),sorted.end());
        size_t index=static_cast<size_t>(ceil(fraction*sorted.size()))-1;
        return sorted[min(index,sorted.size()-1)];
    }

    double minimum() const {
        return samples.empty()?0.0:*min_element(samples.begin(),samples.end());
    }

    double maximum() const {
        return samples.empty()?0.0:*max_element(samples.begin(),samples.end());
    }
};

struct BenchmarkStats {
    TimingSeries boardPreparation;
    TimingSeries search;
    TimingSeries placementInput;
    TimingSeries renderWait;
    TimingSeries screenCapture;
    TimingSeries gridRead;
    TimingSeries queueRead;
    TimingSeries lookingTotal;
    TimingSeries placingTotal;
    TimingSeries fullCycle;
    string activeStage;
    chrono::steady_clock::time_point activeStageStart;

    void beginStage(const string& name) {
        activeStage=name;
        activeStageStart=chrono::steady_clock::now();
    }

    double finishStage(TimingSeries& series) {
        double elapsed=chrono::duration<double,milli>(
            chrono::steady_clock::now()-activeStageStart
        ).count();
        series.add(elapsed);
        activeStage.clear();
        return elapsed;
    }

    double activeStageElapsed() const {
        if(activeStage.empty()) return 0.0;
        return chrono::duration<double,milli>(
            chrono::steady_clock::now()-activeStageStart
        ).count();
    }

    static void printSeries(const string& name,const TimingSeries& series) {
        cout<<left<<setw(22)<<name<<right
            <<setw(10)<<series.samples.size()
            <<setw(12)<<series.average()
            <<setw(12)<<series.percentile(0.50)
            <<setw(12)<<series.percentile(0.95)
            <<setw(12)<<series.minimum()
            <<setw(12)<<series.maximum()<<'\n';
    }

    static string csvEscape(string value) {
        size_t position=0;
        while((position=value.find('"',position))!=string::npos) {
            value.insert(position,"\"");
            position+=2;
        }
        return '"'+value+'"';
    }

    void appendCsv(const string& reason) const {
        filesystem::path path=executableDirectory()/"benchmark_results.csv";
        error_code fileError;
        bool needsHeader=!filesystem::exists(path,fileError)
            || filesystem::file_size(path,fileError)==0;
        ofstream output(path,ios::app);
        if(!output) {
            cerr<<"Could not append benchmark results to "<<path.string()<<'\n';
            return;
        }

        time_t now=time(nullptr);
        tm localTime{};
        localtime_s(&localTime,&now);
        ostringstream timestamp;
        timestamp<<put_time(&localTime,"%Y-%m-%d %H:%M:%S");

        if(needsHeader) {
            output
                <<"timestamp,termination,completed_moves,lookahead_pieces,"
                <<"input_delay_ms,render_wait_target_ms,failed_stage,"
                <<"avg_prepare_ms,avg_search_ms,avg_input_ms,avg_render_wait_ms,"
                <<"avg_capture_ms,avg_grid_read_ms,avg_queue_read_ms,"
                <<"avg_looking_ms,avg_placing_ms,avg_cycle_ms,p95_cycle_ms,avg_pps\n";
        }
        output<<csvEscape(timestamp.str())<<','
              <<csvEscape(reason)<<','
              <<fullCycle.samples.size()<<','
              <<queuedPiecesToLookAhead<<','
              <<inputDelayMs<<','
              <<screenUpdateDelayMs<<','
              <<csvEscape(activeStage)<<','
              <<boardPreparation.average()<<','
              <<search.average()<<','
              <<placementInput.average()<<','
              <<renderWait.average()<<','
              <<screenCapture.average()<<','
              <<gridRead.average()<<','
              <<queueRead.average()<<','
              <<lookingTotal.average()<<','
              <<placingTotal.average()<<','
              <<fullCycle.average()<<','
              <<fullCycle.percentile(0.95)<<','
              <<(fullCycle.average()>0.0?1000.0/fullCycle.average():0.0)<<'\n';
        cout<<"Benchmark row appended to "<<path.string()<<'\n';
    }

    void printSummary(const string& reason) const {
        cout<<"\n================ BENCHMARK SUMMARY ================\n";
        cout<<"Termination: "<<reason<<'\n';
        cout<<"Completed moves: "<<fullCycle.samples.size()<<'\n';
        if(!activeStage.empty()) {
            cout<<"Interrupted stage: "<<activeStage
                <<" ("<<activeStageElapsed()<<" ms before failure)\n";
        }
        cout<<fixed<<setprecision(3);
        cout<<left<<setw(22)<<"Stage"<<right
            <<setw(10)<<"Samples"
            <<setw(12)<<"Average"
            <<setw(12)<<"Median"
            <<setw(12)<<"P95"
            <<setw(12)<<"Min"
            <<setw(12)<<"Max"<<'\n';
        printSeries("Board preparation",boardPreparation);
        printSeries("Search",search);
        printSeries("Placement input",placementInput);
        printSeries("Render wait",renderWait);
        printSeries("Screen capture",screenCapture);
        printSeries("Grid read",gridRead);
        printSeries("Queue read",queueRead);
        printSeries("Looking total",lookingTotal);
        printSeries("Placing total",placingTotal);
        printSeries("Full cycle",fullCycle);
        if(fullCycle.average()>0.0) {
            cout<<"Average throughput: "<<(1000.0/fullCycle.average())<<" pieces/s\n";
        }
        cout<<"===================================================\n";
        appendCsv(reason);
    }
};

BenchmarkStats benchmarkStats;

struct color{
    int R,G,B;
    color(int R,int G,int B):R(R),G(G),B(B){}
};
struct Piece{
    int ind=0;
    color c;
    //cells[0] is the original rotation, up or x will make it cells[1]
    vector<vector<pair<int,int>>>cells;
    char type;
    Piece(int ind,color c,vector<vector<pair<int,int>>>cells,char type):ind(ind),c(c),cells(cells),type(type){}
};
vector<Piece>curQueue;
vector<vector<int>>grid;
//handling rotations will suck so much
//I can just assume they are new pieces basically
Piece IPiece(0,{124,254,198},{{{-1,0},{0,0},{1,0},{2,0}},{{1,1},{1,0},{1,-1},{1,-2}}},'I');
Piece JPiece(1,{148,144,222},{{{0,0},{1,0},{-1,0},{-1,-1}},{{0,0},{0,1},{0,-1},{1,-1}},{{1,1},{0,0},{1,0},{-1,0}},{{0,0},{-1,0},{0,-1},{0,-2}}},'J');
Piece LPiece(2,{250,165,126},{{{0,0},{1,0},{-1,0},{1,-1}},{{0,0},{1,0},{0,-1},{0,-2}},{{0,0},{1,0},{-1,0},{-1,1}},{{0,1},{0,0},{0,-1},{-1,-1}}},'L');
Piece OPiece(3,{255,227,130},{{{0,0},{1,0},{0,-1},{1,-1}}},'O');
Piece ZPiece(4,{252,137,143},{{{0,0},{1,0},{0,-1},{-1,-1}},{{0,0},{1,0},{0,1},{1,-1}}},'Z');
Piece SPiece(5,{196,250,136},{{{0,0},{-1,0},{0,-1},{1,-1}},{{0,0},{0,-1},{1,0},{1,1}}},'S');
Piece TPiece(6,{231,135,211},{{{0,0},{-1,0},{1,0},{0,-1}},{{0,0},{0,1},{0,-1},{1,0}},{{0,0},{-1,0},{1,0},{0,1}},{{0,0},{0,1},{0,-1},{-1,0}}},'T');
Piece all_p[7]={IPiece,JPiece,LPiece,OPiece,ZPiece,SPiece,TPiece};
char all_pc[7]={'I','J','L','O','Z','S','T'};
Piece curPiece=IPiece;

void appendKeyPress(vector<INPUT>& events,WORD keyCode)
{
    INPUT keyDown{};
    keyDown.type=INPUT_KEYBOARD;
    keyDown.ki.wVk=keyCode;
    events.push_back(keyDown);

    INPUT keyUp=keyDown;
    keyUp.ki.dwFlags=KEYEVENTF_KEYUP;
    events.push_back(keyUp);
}
void sendKeySequence(const vector<WORD>& keyCodes)
{
    vector<INPUT> events;
    events.reserve(keyCodes.size()*2);
    for(WORD keyCode:keyCodes)
    {
        appendKeyPress(events,keyCode);
    }

    UINT sent=SendInput(
        static_cast<UINT>(events.size()),
        events.data(),
        sizeof(INPUT)
    );
    if(sent!=events.size())
    {
        ostringstream error;
        error<<"SendInput queued "<<sent<<" of "<<events.size()
             <<" keyboard events (Windows error "<<GetLastError()<<")";
        throw runtime_error(error.str());
    }
}
void preciseWait(DWORD milliseconds)
{
    if(milliseconds==0) return;
    if(highResolutionTimer)
    {
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart=-static_cast<LONGLONG>(milliseconds)*10000;
        if(SetWaitableTimer(highResolutionTimer,&dueTime,0,nullptr,nullptr,FALSE))
        {
            WaitForSingleObject(highResolutionTimer,INFINITE);
            return;
        }
    }
    Sleep(milliseconds);
}
void waitForKeyRelease(int virtualKey)
{
    while(GetAsyncKeyState(virtualKey)&0x8000)
    {
        preciseWait(1);
    }
}

void capture()
{
    BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, x, y, SRCCOPY);
    // Pixel buffer is ready in pPixels (BGRA format)
    pixels = (unsigned int*)pPixels;
}
void configureCaptureRegion()
{
    x=min(screenLayout.board.left,screenLayout.nextQueue.left);
    y=min(screenLayout.board.top,screenLayout.nextQueue.top);
    width=max(screenLayout.board.right,screenLayout.nextQueue.right)-x;
    height=max(screenLayout.board.bottom,screenLayout.nextQueue.bottom)-y;
}
void init()
{
    // Get screen DC
    hScreenDC = GetDC(NULL);
    hMemoryDC = CreateCompatibleDC(hScreenDC);

    // Create DIB section (fast pixel access)
    bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // negative so origin is top-left
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;    // RGBA 32-bit
    bmi.bmiHeader.biCompression = BI_RGB;
    
    
    hBitmap = CreateDIBSection(hMemoryDC, &bmi, DIB_RGB_COLORS, &pPixels, NULL, 0);
    SelectObject(hMemoryDC, hBitmap);
    highResolutionTimer=CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );
    if(!highResolutionTimer)
    {
        highResolutionTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    }
}
void cleanupCapture()
{
    if(highResolutionTimer)
    {
        CloseHandle(highResolutionTimer);
        highResolutionTimer=nullptr;
    }
    if(hMemoryDC)
    {
        DeleteDC(hMemoryDC);
        hMemoryDC=nullptr;
    }
    if(hBitmap)
    {
        DeleteObject(hBitmap);
        hBitmap=nullptr;
    }
    if(hScreenDC)
    {
        ReleaseDC(NULL,hScreenDC);
        hScreenDC=nullptr;
    }
    pPixels=nullptr;
    pixels=nullptr;
}
color getPixelRel(int px,int py)
{
    unsigned int cc = pixels[py * width + px];
    unsigned char blue  = (cc & 0xFF);
    unsigned char green = (cc >> 8) & 0xFF;
    unsigned char red   = (cc >> 16) & 0xFF;
    return color(red,green,blue);
}
color getPixelSlow(int px,int py)
{
    COLORREF c = GetPixel(hScreenDC, px, py);
    int red = GetRValue(c);
    int green = GetGValue(c);
    int blue = GetBValue(c);
    return color(red,green,blue);
}
color getPixelAbs(int px,int py)
{
    return getPixelRel(px-x,py-y);
}
//will make it get avg instead
int cc=0;
color getMaxInRegion(int tx,int ty,int bx,int by)
{
    color ret(0,0,0);
    tx=max(tx,0);
    ty=max(ty,0);
    bx=min(bx,width-1);
    by=min(by,height-1);
    for(int i=tx;i<=bx;i++)
    {
        for(int j=ty;j<=by;j++)
        {
            color cur = getPixelRel(i,j);
            ret.R=max(ret.R,cur.R);
            ret.G=max(ret.G,cur.G);
            ret.B=max(ret.B,cur.B);
        }
    }
    

    return ret;
}

int getColorDistance(color first,color second)
{
    int red=first.R-second.R;
    int green=first.G-second.G;
    int blue=first.B-second.B;
    return red*red+green*green+blue*blue;
}
int getNearestPieceColorDistance(color sampledColor)
{
    int bestDistance=numeric_limits<int>::max();
    for(const Piece& piece:all_p)
    {
        bestDistance=min(bestDistance,getColorDistance(sampledColor,piece.c));
    }
    return bestDistance;
}
color getRepresentativePieceColor(int tx,int ty,int bx,int by)
{
    color bestColor(0,0,0);
    int bestDistance=numeric_limits<int>::max();
    for(int px=tx;px<=bx;px+=queuePixelStride)
    {
        for(int py=ty;py<=by;py+=queuePixelStride)
        {
            color sampledColor=getPixelRel(px,py);
            int highest=max({sampledColor.R,sampledColor.G,sampledColor.B});
            int lowest=min({sampledColor.R,sampledColor.G,sampledColor.B});

            // Ignore the black background and white/gray panel borders.
            if(highest<50 || highest-lowest<20)
            {
                continue;
            }

            int distance=getNearestPieceColorDistance(sampledColor);
            if(distance<bestDistance)
            {
                bestDistance=distance;
                bestColor=sampledColor;
            }
        }
    }
    return bestColor;
}
vector<color>getQueueColors()
{
    vector<color>ret;
    int tx=screenLayout.nextQueue.left-x;
    int bx=screenLayout.nextQueue.right-x-1;
    int queueTop=screenLayout.nextQueue.top-y;
    int queueHeight=screenLayout.nextQueue.height();
    for(int i=0;i<queueLength;i++)
    {
        int ty=queueTop+(queueHeight*i)/queueLength;
        int by=queueTop+(queueHeight*(i+1))/queueLength-1;
        ret.push_back(getRepresentativePieceColor(tx,ty,bx,by));
    }
    return ret;
}
Piece getPieceByColor(color c)
{
    int mx=1000000;
    int mxi=0;
    for(int i=0;i<7;i++)
    {
        int diff=getColorDistance(c,all_p[i].c);
        if(diff<mx)
        {
            mx=diff,mxi=i;
        }
    }
    return all_p[mxi];
}
vector<Piece>getQueue(vector<color>* sampledColors=nullptr)
{
    vector<Piece>ret;
    vector<color>colors=getQueueColors();
    if(sampledColors)
    {
        *sampledColors=colors;
    }
    for(const color& sampledColor:colors)
    {
        ret.push_back(getPieceByColor(sampledColor));
    }
    return ret;

}
string getQueueTypes(const vector<Piece>& queue)
{
    string types;
    for(const Piece& piece:queue)
    {
        types.push_back(piece.type);
    }
    return types;
}
bool hasQueueAdvanced(const vector<Piece>& previous,const vector<Piece>& current)
{
    if(previous.size()!=queueLength || current.size()!=queueLength)
    {
        return false;
    }
    for(int i=0;i+1<queueLength;i++)
    {
        if(current[i].type!=previous[i+1].type)
        {
            return false;
        }
    }
    return true;
}
struct UpdatedFrame {
    vector<Piece> queue;
    vector<color> queueColors;
    double renderWaitMs=0;
    double captureMs=0;
    double queueReadMs=0;
    int captureAttempts=0;
};
UpdatedFrame waitForUpdatedFrame(const vector<Piece>& previousQueue)
{
    UpdatedFrame result;
    auto synchronizationStart=chrono::steady_clock::now();
    benchmarkStats.activeStage="render synchronization";
    benchmarkStats.activeStageStart=synchronizationStart;

    while(true)
    {
        auto waitStart=chrono::steady_clock::now();
        preciseWait(screenUpdateDelayMs);
        result.renderWaitMs+=chrono::duration<double,milli>(
            chrono::steady_clock::now()-waitStart
        ).count();

        auto captureStart=chrono::steady_clock::now();
        capture();
        result.captureMs+=chrono::duration<double,milli>(
            chrono::steady_clock::now()-captureStart
        ).count();
        result.captureAttempts++;

        auto queueStart=chrono::steady_clock::now();
        result.queue=getQueue(&result.queueColors);
        result.queueReadMs+=chrono::duration<double,milli>(
            chrono::steady_clock::now()-queueStart
        ).count();

        if(hasQueueAdvanced(previousQueue,result.queue))
        {
            benchmarkStats.activeStage.clear();
            return result;
        }

        double elapsed=chrono::duration<double,milli>(
            chrono::steady_clock::now()-synchronizationStart
        ).count();
        if(elapsed>=screenUpdateTimeoutMs)
        {
            ostringstream error;
            error<<"Timed out waiting for NEXT queue to advance after "
                 <<elapsed<<" ms; previous="<<getQueueTypes(previousQueue)
                 <<", last seen="<<getQueueTypes(result.queue);
            throw runtime_error(error.str());
        }
    }
}
bool SaveBMP(const char* filename, void* pPixels, int width, int height) {
    BITMAPFILEHEADER fileHeader;
    BITMAPINFOHEADER infoHeader;

    int imageSize = width * height * 4; // 4 bytes per pixel (BGRA)

    // File header
    fileHeader.bfType = 0x4D42; // "BM"
    fileHeader.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize;
    fileHeader.bfReserved1 = 0;
    fileHeader.bfReserved2 = 0;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    // Info header
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = width;
    infoHeader.biHeight = -height; // BMP expects bottom-up rows (positive height)
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = imageSize;
    infoHeader.biXPelsPerMeter = 0;
    infoHeader.biYPelsPerMeter = 0;
    infoHeader.biClrUsed = 0;
    infoHeader.biClrImportant = 0;

    // Write file
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;

    file.write((const char*)&fileHeader, sizeof(fileHeader));
    file.write((const char*)&infoHeader, sizeof(infoHeader));
    file.write((const char*)pPixels, imageSize);

    return true;
}
//cell width is 35
//cell height should be 35
//cx/35 should be its index

//I will make 0th row the top row
//row 20 is the floor, it is always filled with 1s
//for clarityp
//Pieces are defined by center piece, and positions of other pieces relative to it
int unsolvableCells[30][15];
int isIDep[10];
vector<int>firstInCol(10,0);
bool debugScore=0;
void load_grid()
{
    const double cellWidth=static_cast<double>(screenLayout.board.width())/boardColumns;
    const double cellHeight=static_cast<double>(screenLayout.board.height())/boardRows;
    const int boardLeft=screenLayout.board.left-x;
    const int boardTop=screenLayout.board.top-y;
    for(int row=0;row<boardRows;row++)
    {
        for(int column=0;column<boardColumns;column++)
        {
            int centerX=boardLeft+static_cast<int>(lround((column+0.5)*cellWidth));
            int centerY=boardTop+static_cast<int>(lround((row+0.5)*cellHeight));
            color cur=getMaxInRegion(
                centerX-cellSampleRadius,
                centerY-cellSampleRadius,
                centerX+cellSampleRadius,
                centerY+cellSampleRadius
            );
            if(cur.R>34 || cur.G>34 || cur.B>34)
            {
                grid[row][column] = 1;
            }
            else
            {
                grid[row][column] = 0;
            }
        }
    }
}
void popPiece(int i,int j,int rot, Piece p)
{
    for(int k=0;k<p.cells[rot].size();k++)
    {
        int ni = i+p.cells[rot][k].second;
        int nj = j+p.cells[rot][k].first;
        grid[ni][nj]=0;
        ///Reminder that this won't update the firstInCol..
    }
}
void pushPiece(int i,int j,int rot, Piece p)
{
    
    for(int k=0;k<p.cells[rot].size();k++)
    {
        int ni = i+p.cells[rot][k].second;
        int nj = j+p.cells[rot][k].first;
        bool inBounds=ni>=0 && ni<boardRows && nj>=0 && nj<boardColumns;
        if(!inBounds || grid[ni][nj]!=0)
        {
            ostringstream error;
            error<<"Invalid simulated placement: piece="<<p.type
                 <<", center=("<<i<<", "<<j<<")"
                 <<", rotation="<<rot
                 <<", cell=("<<ni<<", "<<nj<<")";
            if(inBounds)
            {
                error<<", occupied="<<grid[ni][nj];
            }
            throw runtime_error(error.str());
        }
        grid[ni][nj]=1;
        firstInCol[nj]=max(firstInCol[nj],20-ni);
    }
}
//TODO (important) optimize this as much as possible
//idea, I can get the lowest row by finding the max in each column
int getLowestRow(int j, int rot, Piece p)
{
    //j is the position for the "core" of the piece
    //I need to put it as down as possible
    int to_put=20;
    ///HERE I also reduce space from top of grid..
    //this should be really optimizable
    bool valid=1;
    for(int k=0;k<p.cells[rot].size();k++)
    {
        
        int nj = j+p.cells[rot][k].first;
        if(nj>=10 || nj<0)
        {
            return -1;
        }
        to_put=min(to_put,19-firstInCol[nj]-p.cells[rot][k].second);
        /*
        0 0 0 0 0 x x 0 0 0 (17th) 4
        0 0 0 0 0 1 x x 0 0 (18th) 2
        0 1 1 0 0 1 0 0 0 0 (19th) 1
        1 1 1 1 1 1 1 1 1 1 (20th) 0
        */
    }
    
    return to_put;
}
//ASSUMPTIONS
//O has 0 rotations
//I,Z,S have two rotations
//(although they act differently)
//J,L,T have 4 rotations
int cnt=0;
filesystem::path saveDebugCapture()
{
    filesystem::path path=executableDirectory()/("debug_capture_"+to_string(cnt++)+".bmp");
    if(!SaveBMP(path.string().c_str(),pPixels,width,height))
    {
        cerr<<"Could not save debug capture: "<<path.string()<<endl;
        return {};
    }
    return path;
}
void printDebugSnapshot(
    const string& label,
    const vector<Piece>& queue,
    const vector<color>& queueColors,
    optional<char> currentPieceType=nullopt
)
{
    const filesystem::path capturePath=saveDebugCapture();
    cout<<"\n=== DEBUG SNAPSHOT: "<<label<<" ===\n";
    if(!capturePath.empty())
    {
        cout<<"Captured image: "<<capturePath.string()<<'\n';
    }
    cout<<"Capture rect: ("<<x<<", "<<y<<") "<<width<<" x "<<height<<'\n';
    cout<<"Board rect:   ("<<screenLayout.board.left<<", "<<screenLayout.board.top
        <<") "<<screenLayout.board.width()<<" x "<<screenLayout.board.height()<<'\n';
    cout<<"NEXT rect:    ("<<screenLayout.nextQueue.left<<", "<<screenLayout.nextQueue.top
        <<") "<<screenLayout.nextQueue.width()<<" x "<<screenLayout.nextQueue.height()<<'\n';
    if(currentPieceType)
    {
        cout<<"Tracked current piece: "<<*currentPieceType<<'\n';
    }

    cout<<"\nDetected board (# = filled, . = empty)\n";
    cout<<"    0123456789\n";
    for(int row=0;row<boardRows;row++)
    {
        cout<<setw(2)<<setfill('0')<<row<<"  ";
        cout<<setfill(' ');
        for(int column=0;column<boardColumns;column++)
        {
            cout<<(grid[row][column]?'#':'.');
        }
        cout<<'\n';
    }

    cout<<"\nDetected NEXT queue (top to bottom)\n";
    for(size_t i=0;i<queue.size() && i<queueColors.size();i++)
    {
        const color& sampled=queueColors[i];
        cout<<i<<": "<<queue[i].type
            <<"  RGB=("<<sampled.R<<", "<<sampled.G<<", "<<sampled.B<<")"
            <<"  distance="<<getColorDistance(sampled,queue[i].c)<<'\n';
    }
    cout<<"=== END DEBUG SNAPSHOT ===\n"<<endl;
}
size_t actuallyPutThePiece(int pos,int rotateCount)
{
    vector<WORD> keyCodes;
    keyCodes.reserve(8);
    int curPos=4;
    if(rotateCount == 3)
    {
        keyCodes.push_back('Z');
        rotateCount=0;
    }
    if(rotateCount == 2)
    {
        keyCodes.push_back('A');
        rotateCount=0;
    }
    while(rotateCount>0)
    {
        keyCodes.push_back(VK_UP);
        rotateCount--;
    }
    while(curPos>pos)
    {
        keyCodes.push_back(VK_LEFT);
        curPos--;
    }
    while(curPos<pos)
    {
        keyCodes.push_back(VK_RIGHT);
        curPos++;
    }
    keyCodes.push_back(VK_SPACE);
    sendKeySequence(keyCodes);
    return keyCodes.size();
}

///SOME heuritics for score
///Score will be penalty, goal is to minimize it
//minimize stack height
//minimize unsolvable holes
//minimize stack height difference
//minimize stairs
//minimize dependencies (especially I's)
//minimize holes
///todo(done): give weight for each criteria (and actually implement them)

//TODO: optimize this

int getScoreOfGrid()
{
    int mx=0,mn=-1;
    int numberOfHoles=0;
    memset(unsolvableCells,0,sizeof(unsolvableCells));
    memset(isIDep,0,sizeof(isIDep));
    int aboveHoles=0;
    for(int i=2;i<20;i++)
    {
        for(int j=0;j<10;j++)
        {
            //remember that you removed the right side unsolvable check
            if(!grid[i][j] && ((grid[i-1][j]) || unsolvableCells[i-1][j]))
            {
                unsolvableCells[i][j]=1;
                numberOfHoles++;
            }
            mx=max(mx,firstInCol[j]);
            if(i==19 && (mn == -1 || firstInCol[j]<mn))mn=firstInCol[j];
        }
    }
    mn=max(mn,0);
    
    for(int i=18;i>=0;i--)
    {
        for(int j=0;j<10;j++){
            if(grid[i][j] && unsolvableCells[i+1][j])
            {
                unsolvableCells[i][j]=1;
                aboveHoles++;
            }
        }
    }
    //a hole is an I dep if 3 tall on both sides
    //but if all I dep are in the same column then its fine

    int cntIDep=0;
    
    int sm=0;
    for(int j=0;j<10;j++)
    {
        
        sm+=((firstInCol[j]-mn+1) * (max(1,(firstInCol[j]-mn+1)-5)));
        for(int i=6;i<20;i++)
        {
            for(int k=0;k<4;k++)
            {
                if((j==0 || grid[i-k][j-1]==1)&& (grid[i-k][j]==0) && grid[i-k][j+1]==1)
                {
                    isIDep[j]++;
                    cntIDep+=(isIDep[j]>=3);
                    if(isIDep[j]>=3)isIDep[j]=0;
                }
                else
                {
                    isIDep[j]=0;
                    break;
                }
            }
        }
    }
    //spikes..
    if(debugScore){
        cout<<cntIDep<<" "<<numberOfHoles<<" "<<aboveHoles<<" "<<mx<<" "<<sm<<" "<<mn<<endl;
        for(int j=0;j<10;j++)cout<<firstInCol[j]<<" ";
        cout<<endl;
    }
    int score = sm + numberOfHoles*50 + aboveHoles + mx;
    return score;
}
//TODO: somehow optmize this
int clear_all_grid()
{
    firstInCol = vector<int>(10,0);
    int ret=0;
    vector<int>rows;
    for(int i=19;i>=0;i--)
    {
        int c=0;
        for(int j=0;j<10;j++)c+=grid[i][j];
        if(c<10)rows.push_back(i);
        else
        {
            ret++;
            for(int j=0;j<10;j++)grid[i][j]=0;
        }
    }
    int it=0;
    int cpyrow=0;
    for(int i=19;i>=0;i--)
    {
        if(it<rows.size())
        {
            for(int j=0;j<10;j++)
            {
                grid[i][j]=grid[rows[it]][j];
                if(grid[i][j])firstInCol[j]=max(firstInCol[j],20-i);
            }
            it++;
        }
        else
        {
            for(int j=0;j<10;j++)grid[i][j]=0;
        }
    }
    return ret;

}
stack<vector<vector<int>>>grids;
stack<vector<int>>firstInCols;
void saveGrid()
{
    grids.push(grid);
    firstInCols.push(firstInCol);
}
void resetGrid()
{
    grid=grids.top();
    firstInCol=firstInCols.top();
}
void unsaveGrid()
{
    grids.pop();
    firstInCols.pop();
}
vector<pair<int,int>>getMoves(Piece &p)
{
    vector<pair<int,int>>ret;
    for(int rot=0;rot<p.cells.size();rot++)
    {
        for(int j=-1;j<10;j++)
        {
            int x=getLowestRow(j,rot,p);
            if(x!=-1)
            {
                ret.push_back({j,rot});
            }
        }
    }
    return ret;
}
vector<array<int,3>>retV;
array<int,3> getBestPos(Piece &p,int curDepth)
{
    if(curDepth>maxDepth)
    {
        return {0,0,getScoreOfGrid()};
    }
    int bestScore=10000000;
    int mnHeightInd=4;
    int mnHeighRot=0;
    saveGrid();
    vector<pair<int,int>>moves = getMoves(p);
     
    for(int i=0;i<moves.size();i++)
    {
        int j=moves[i].first;
        int rot=moves[i].second;
        int x=getLowestRow(j,rot,p);
        if(x!=-1)
        {
            pushPiece(x,j,rot,p);
            //this is awkward, I should be putting this inside the scoring function
            int cst=clear_all_grid();
            array<int,3>cur=getBestPos(curQueue[curDepth],curDepth+1);
            int curScore = cur[2] + -10*cst*cst*cst*cst;
            if(curDepth == 0)
            {
                retV.push_back({j,rot,curScore});
            }
            if(curScore<bestScore)
            {
                bestScore=curScore;
                mnHeightInd=j;
                mnHeighRot=rot;
            }
            resetGrid();
        }
    }
    unsaveGrid();
    return {mnHeightInd,mnHeighRot,bestScore};
}
bool cmp(array<int,3>&a,array<int,3>&b)
{
    if(a[2]==b[2]){
        if(a[1]==b[1])return a[0]<b[0];
        return a[1]<b[1];
    }
    return a[2]<b[2];
}
array<int,3> getBestPosIterative(Piece p)
{
    maxDepth=realMaxDepth;
    retV.clear();
    return getBestPos(p,0);
}
vector<pair<int,int>>movesForPiece[7];
void preCompMoves()
{
    for(int i=0;i<7;i++)
        movesForPiece[i]=getMoves(all_p[i]);
}
vector<vector<pair<int,int>>>placements;
void getPlacements(int depth,vector<pair<int,int>>&curMoves)
{
    if(depth>maxDepth)
    {
        placements.push_back(curMoves);
        return;
    }
    int indOfPiece=0;
    if(depth == 0)
    {
        indOfPiece=curPiece.ind;
    }
    else
    {
        indOfPiece=curQueue[depth-1].ind;
    }
    for(auto i:movesForPiece[indOfPiece])
    {
        curMoves.push_back(i);
        getPlacements(depth+1,curMoves);
        curMoves.pop_back();
    }
    
}
array<int,3> bruteForceOnAll()
{
    maxDepth=realMaxDepth;
    placements.clear();
    vector<pair<int,int>>temp;
    //cout<<"Getting placements.."<<endl;
    getPlacements(0,temp);
    /*cout<<"Done"<<endl;
    for(int i=0;i<placements.size();i++)
    {
        for(int j=0;j<placements[i].size();j++)
        {
            cout<<placements[i][j].first<<" "<<placements[i][j].second<<endl;
        }
        cout<<endl;
    }
    cout<<endl;
    cout<<curPiece.type<<endl;*/


    array<int,3>ret = {0,0,(int)1e9};
    saveGrid();
    for(int i=0;i<placements.size();i++)
    {
        int total_clr_cost=0;
        for(int j=0;j<placements[i].size();j++)
        {
            int pieceInd = curPiece.ind;
            if(j)pieceInd = curQueue[j-1].ind;
            pushPiece(getLowestRow(placements[i][j].first,placements[i][j].second,all_p[pieceInd])
            ,placements[i][j].first,placements[i][j].second,all_p[pieceInd]);
            int cst=clear_all_grid();
            cst = -10*cst*cst*cst*cst;
            total_clr_cost+=cst;
        }
        int score=getScoreOfGrid()+total_clr_cost;
       // cout<<"algo2: "<<placements[i][0].first<<" "<<placements[i][0].second<<" "<<score<<endl;
        
        if(score<ret[2])
        {
            ret[2]=score;
            ret[0]=placements[i][0].first;
            ret[1]=placements[i][0].second;
        }
        resetGrid();
    }
    unsaveGrid();
    return ret;
    
}
double dp(int depth)
{
    if(depth == 0)return 1;
    double ret=0;
    for(int i=0;i<7;i++)
    {
        int cnt=0;
        for(int j=-1;j<10;j++)
        {
            for(int rot=0;rot<all_p[i].cells.size();rot++)
            {
                if(getLowestRow(j,rot,all_p[i])!=-1)
                {
                    cnt++;
                }
            }
        }
        ret+=1.00/7.00 * (dp(depth-1))*(double)cnt;
    }
    return ret;
}
int runBot(int argc,char* argv[]) {
    ios_base::sync_with_stdio(0);
    cin.tie(0);
    bool debugMode=false;
    bool inspectOnly=false;
    for(int i=1;i<argc;i++)
    {
        string argument=argv[i];
        if(argument=="--debug")
        {
            debugMode=true;
        }
        else if(argument=="--inspect")
        {
            debugMode=true;
            inspectOnly=true;
        }
        else if(argument=="--help")
        {
            cout<<"Usage: color.exe [--debug | --inspect]\n"
                <<"  --debug    Run the bot and log every captured state.\n"
                <<"  --inspect  Capture and log one state without playing.\n";
            return 0;
        }
        else
        {
            cerr<<"Unknown option: "<<argument<<"\nUse --help to list options."<<endl;
            return 1;
        }
    }
    string layoutError;
    const auto layoutPath=defaultScreenLayoutPath();
    if(!loadScreenLayout(layoutPath,screenLayout,layoutError))
    {
        cerr<<layoutError<<'\n';
        cerr<<"Build and run calibrate.cpp before starting the bot."<<endl;
        return 1;
    }
    configureCaptureRegion();
    vector<int>zeros(11,0);
    for(int i=0;i<21;i++)grid.push_back(zeros);
    for(int j=0;j<10;j++)
    {
        grid[20][j]=1;
    }
    for(int i=0;i<20;i++)
    {
        grid[i][10]=1;
    }
    //preCompMoves();
   /* for(int i=0;i<7;i++)
    {
        cout<<"i: "<<i<<endl;
        for(auto j:movesForPiece[i])cout<<j.first<<" "<<j.second<<endl;
    }*/
    /*for(int j=0;j<=6;j++)cout<<dp(j)<<endl;
    
    
    cout<<endl;
    return 0;*/
   /* for(int i=0;i<20;i++)
    {
        for(int j=0;j<10;j++)
        {
            cin>>grid[i][j];
        }
    }
    cout<<getScoreOfGrid()<<endl;
    cout<<endl<<endl;
    //return 0;
    clear_all_grid();
    cout<<"first in col: ";
    for(int i=0;i<10;i++)cout<<firstInCol[i]<<" ";
    cout<<endl;
    saveGrid();
    
    for(int i=0;i<7;i++)
    {
        cout<<"!!!!!!!!!i: "<<i<<endl;
        for(int j=-1;j<10;j++)
        {
            cout<<j<<":";
            for(int rot=0;rot<all_p[i].cells.size();rot++)
            {
                cout<<getLowestRow(j,rot,all_p[i])<<" ";
            }
            cout<<endl;
        }
        cout<<endl;
    }
    //TIL that I was redeclaring the global cur piece which was making algo 2 different than algo 1
    //2 hours for this wow...
    return 0;*/
    init();
    //I will need to use the extra space at the top later, will implement as if I don't need it tho rn
    srand(2);
    
    if (!pPixels) {
        std::cerr << "Failed to create DIB section" << std::endl;
        cleanupCapture();
        return -1;
    }
    cout<<(inspectOnly?"Inspection mode.":"Bot ready.")<<endl;
    cout<<"Press P when the game is visible and ready."<<endl;
    while (true) {
        if (GetAsyncKeyState(0x50) & 0x8000) {
            cout<<(inspectOnly?"Capturing inspection frame...":"Starting game...")<<endl;
            cout.flush();
            break;
        }
        Sleep(inputPollingDelayMs);
    }
    capture();
    load_grid();
    vector<color>queueColors;
    curQueue=getQueue(&queueColors);
    if(debugMode)
    {
        printDebugSnapshot("initial capture",curQueue,queueColors);
    }
    if(inspectOnly)
    {
        cleanupCapture();
        return 0;
    }
    //I need to put a piece first before bot taking over
    while (true) {
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
            // Do not let the first injected hard drop overlap the user's
            // physical Space press. TETR.IO needs a fresh press edge.
            waitForKeyRelease(VK_SPACE);
            preciseWait(initialScreenUpdateDelayMs);
            break;
        }
        Sleep(inputPollingDelayMs);
    }
    curPiece=curQueue[0];
    capture();
    load_grid();
    curQueue=getQueue(&queueColors);
    if(debugMode)
    {
        printDebugSnapshot("after manual first placement",curQueue,queueColors,curPiece.type);
    }
    /*
        Flow of logic should be something like

        1. find best place for current state
        2. put piece in that place
        3. get new statep
    */
    int cur_move=1;
    while (1) 
    {   
        //if I press P stop the bot (fail safe instead of ctrl c from terminal)
        if (GetAsyncKeyState(0x50) & 0x8000) {
            cout<<"done!"<<endl;
            break;
        }
        auto cycleStart=std::chrono::steady_clock::now();
        char playedPiece=curPiece.type;
        benchmarkStats.beginStage("board preparation");
        clear_all_grid();
        double preparationMs=benchmarkStats.finishStage(benchmarkStats.boardPreparation);

        //1
        benchmarkStats.beginStage("search");
        array<int,3>best_play=getBestPosIterative(curPiece);
        double searchMs=benchmarkStats.finishStage(benchmarkStats.search);
       /* for(int i=0;i<retV.size();i++)
        {
            cout<<"algo1: "<<retV[i][0]<<" "<<retV[i][1]<<" "<<retV[i][2]<<endl;
        }
        cout<<curPiece.type<<endl;
        for(int i=0;i<20;i++)
        {
            for(int j=0;j<10;j++)cout<<grid[i][j]<<" ";
            cout<<endl;
        }
        cout<<endl<<endl;*/


        /*start = std::chrono::high_resolution_clock::now();

        array<int,3>best_play2 = bruteForceOnAll();

        end = std::chrono::high_resolution_clock::now();
        duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        double time2 = duration_ms.count();*/


        int moveNumber=cur_move++;
        /*for(int i=0;i<20;i++)
        {
            for(int j=0;j<10;j++)cout<<grid[i][j]<<" ";
            cout<<endl;
        }*/
        //cout<<endl;
        if(debugMode)
        {
            benchmarkStats.beginStage("debug verification");
            pushPiece(getLowestRow(best_play[0],best_play[1],curPiece),best_play[0],best_play[1],curPiece);
            debugScore=1;
            cout<<"score after chosen move: "<<getScoreOfGrid()<<endl;
            debugScore=0;
            benchmarkStats.activeStage.clear();
        }
        
        
        



        //2
        benchmarkStats.beginStage("placement input");
        size_t keyPressCount=actuallyPutThePiece(best_play[0],best_play[1]);
        double placementInputMs=benchmarkStats.finishStage(benchmarkStats.placementInput);

        // Poll captured frames until NEXT has actually shifted. This replaces the
        // fixed render sleep and leaves the final successful frame in pPixels.
        UpdatedFrame updatedFrame=waitForUpdatedFrame(curQueue);
        double renderWaitMs=updatedFrame.renderWaitMs;
        double captureMs=updatedFrame.captureMs;
        double queueReadMs=updatedFrame.queueReadMs;
        benchmarkStats.renderWait.add(renderWaitMs);
        benchmarkStats.screenCapture.add(captureMs);
        benchmarkStats.queueRead.add(queueReadMs);

        benchmarkStats.beginStage("grid read");
        load_grid();
        double gridReadMs=benchmarkStats.finishStage(benchmarkStats.gridRead);
        curPiece=curQueue[0];
        curQueue=move(updatedFrame.queue);
        queueColors=move(updatedFrame.queueColors);
        double lookingMs=captureMs+gridReadMs+queueReadMs;
        double placingMs=placementInputMs+renderWaitMs;
        benchmarkStats.lookingTotal.add(lookingMs);
        benchmarkStats.placingTotal.add(placingMs);
        if(debugMode)
        {
            printDebugSnapshot(
                "after bot move "+to_string(cur_move-1),
                curQueue,
                queueColors,
                curPiece.type
            );
        }
        double cycleMs=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-cycleStart
        ).count();
        benchmarkStats.fullCycle.add(cycleMs);
        cout<<fixed<<setprecision(3);
        cout<<"move: "<<moveNumber<<"  piece: "<<playedPiece
            <<"  position: "<<best_play[0]<<"  rotation: "<<best_play[1]
            <<"  score: "<<best_play[2]
            <<"  keys: "<<keyPressCount<<'\n';
        cout<<"prepare: "<<preparationMs<<" ms"
            <<"  search: "<<searchMs<<" ms"
            <<"  placing: "<<placingMs<<" ms"
            <<"  looking: "<<lookingMs<<" ms\n";
        cout<<"  input: "<<placementInputMs<<" ms"
            <<"  render wait: "<<renderWaitMs<<" ms"
            <<"  capture: "<<captureMs<<" ms"
            <<"  grid: "<<gridReadMs<<" ms"
            <<"  queue: "<<queueReadMs<<" ms"
            <<"  capture attempts: "<<updatedFrame.captureAttempts<<"\n";
        cout<<"full cycle: "<<cycleMs<<" ms"
            <<" ("<<(1000.0/cycleMs)<<" pieces/s)"<<endl;
    }
    benchmarkStats.printSummary("stopped by user");
    cleanupCapture();

    return 0;
}

int main(int argc,char* argv[])
{
    SetProcessDPIAware();
    try
    {
        return runBot(argc,argv);
    }
    catch(const exception& error)
    {
        cerr<<"\nFatal bot error: "<<error.what()<<'\n';
        benchmarkStats.printSummary(string("failure: ")+error.what());
        cleanupCapture();
        return 1;
    }
    catch(...)
    {
        cerr<<"\nFatal bot error: unknown exception\n";
        benchmarkStats.printSummary("failure: unknown exception");
        cleanupCapture();
        return 1;
    }
}
