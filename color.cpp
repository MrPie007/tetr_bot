#include <windows.h>
#include <dwmapi.h>
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
HANDLE compositionFrameTimer;
LARGE_INTEGER performanceCounterFrequency;

constexpr int queuedPiecesToLookAhead=0;
constexpr DWORD inputDelayMs=0;
constexpr DWORD screenUpdateDelayMs=1;
constexpr DWORD screenUpdateTimeoutMs=250;
constexpr DWORD queueBatchSettleDelayMs=5;
constexpr double expectedFrameIntervalMs=5.0;
constexpr double compositionFrameSafetyMarginMs=0.25;
constexpr DWORD boardResyncWarmupMs=1000;
constexpr int boardResyncIntervalMoves=200;
constexpr DWORD inputPollingDelayMs=2;
constexpr int statusPrintIntervalMoves=200;
constexpr int parallelSearchDepthThreshold=3;
// Zero selects the machine's logical CPU count. The offline evaluator sets
// this to one because it already parallelizes independent games.
int compactSearchThreadCount=0;
int maxDepth=queuedPiecesToLookAhead;
int realMaxDepth=queuedPiecesToLookAhead;
constexpr int boardColumns=10;
constexpr int boardRows=20;
constexpr int queueLength=5;
// Keep at least one previously known piece as an overlap when NEXT is
// refreshed. With lookahead 1, one full queue capture therefore supports four
// hard drops before another physical capture is needed.
int queueRefreshBatchSize=queueLength-1;
constexpr int cellSampleRadius=1;
constexpr int queuePixelStride=2;
constexpr int queueSlotVerticalMarginDivisor=6;
constexpr double maximumQueueColorDistance=0.03;
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
    TimingSeries localOnlyCycle;
    TimingSeries queueSyncCycle;
    TimingSeries queueSyncTotal;
    TimingSeries queueInitialWait;
    TimingSeries queueRetryWait;
    TimingSeries queueCaptureWall;
    TimingSeries queueCaptureCpu;
    TimingSeries queueCaptureOffCpu;
    TimingSeries queueDecodePhysical;
    TimingSeries queueSyncOther;
    TimingSeries compositionRefreshInterval;
    size_t queueSynchronizationCount=0;
    size_t compositionClockSynchronizationCount=0;
    size_t queueCaptureAttemptCount=0;
    size_t unreliableQueueFrames=0;
    size_t unchangedQueueFrames=0;
    size_t overlapMismatchFrames=0;
    size_t sevenBagMismatchFrames=0;
    size_t boardResyncCount=0;
    size_t boardDriftCorrectionCount=0;
    size_t correctedBoardCells=0;
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
              <<realMaxDepth<<','
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
        printSeries("Board update",gridRead);
        printSeries("Queue read",queueRead);
        printSeries("Looking total",lookingTotal);
        printSeries("Placing total",placingTotal);
        printSeries("Full cycle",fullCycle);
        printSeries("Local-only cycle",localOnlyCycle);
        printSeries("Queue-sync cycle",queueSyncCycle);
        if(fullCycle.average()>0.0) {
            cout<<"Average throughput: "<<(1000.0/fullCycle.average())<<" pieces/s\n";
        }
        cout<<"Physical NEXT synchronizations: "
            <<queueSynchronizationCount<<'\n';
        if(!queueSyncTotal.samples.empty()) {
            cout<<"\nQueue synchronization detail (not amortized)\n";
            cout<<left<<setw(22)<<"Stage"<<right
                <<setw(10)<<"Samples"
                <<setw(12)<<"Average"
                <<setw(12)<<"Median"
                <<setw(12)<<"P95"
                <<setw(12)<<"Min"
                <<setw(12)<<"Max"<<'\n';
            printSeries("Synchronization total",queueSyncTotal);
            printSeries("Initial settle wait",queueInitialWait);
            printSeries("Retry waits",queueRetryWait);
            printSeries("BitBlt wall",queueCaptureWall);
            printSeries("BitBlt thread CPU",queueCaptureCpu);
            printSeries("BitBlt off-CPU",queueCaptureOffCpu);
            printSeries("Queue decode",queueDecodePhysical);
            printSeries("Other sync overhead",queueSyncOther);
            printSeries("DWM refresh interval",compositionRefreshInterval);
            cout<<"Capture attempts per synchronization: "
                <<static_cast<double>(queueCaptureAttemptCount)
                    /queueSyncTotal.samples.size()<<'\n';
            cout<<"Rejected queue frames: unreliable="
                <<unreliableQueueFrames
                <<", unchanged="<<unchangedQueueFrames
                <<", overlap mismatch="<<overlapMismatchFrames
                <<", seven-bag mismatch="<<sevenBagMismatchFrames<<'\n';
            cout<<"DWM-clock synchronizations: "
                <<compositionClockSynchronizationCount<<'/'
                <<queueSynchronizationCount<<'\n';
        }
        cout<<"Physical board resyncs: "<<boardResyncCount<<'\n';
        cout<<"Board drift corrections: "<<boardDriftCorrectionCount
            <<" ("<<correctedBoardCells<<" differing cells)\n";
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
// TETR.IO's block shading makes the captured colors considerably darker than
// the old palette values.  In particular, the old O reference was close enough
// to L in raw RGB space that every O in the queue was being read as L.
Piece OPiece(3,{195,171,65},{{{0,0},{1,0},{0,-1},{1,-1}}},'O');
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
    if(events.empty()) return;

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
bool armCompositionFrameTimer(int refreshCount,double& refreshIntervalMs)
{
    refreshIntervalMs=0;
    if(!compositionFrameTimer || performanceCounterFrequency.QuadPart<=0
        || refreshCount<=0)
    {
        return false;
    }

    DWM_TIMING_INFO timing{};
    timing.cbSize=sizeof(timing);
    if(FAILED(DwmGetCompositionTimingInfo(nullptr,&timing))
        || timing.qpcRefreshPeriod==0)
    {
        return false;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    refreshIntervalMs=
        static_cast<double>(timing.qpcRefreshPeriod)*1000.0
            /performanceCounterFrequency.QuadPart;
    // Do not sample the queue while the target refresh is still being
    // composed. A small post-boundary margin prevents a transitional queue
    // from being accepted when adjacent queue pieces have the same type.
    long double safetyTicksExact=
        compositionFrameSafetyMarginMs
            *performanceCounterFrequency.QuadPart/1000.0L;
    ULONGLONG safetyTicks=
        static_cast<ULONGLONG>(ceil(safetyTicksExact));
    ULONGLONG targetRefreshQpc=timing.qpcVBlank
        +static_cast<ULONGLONG>(refreshCount)*timing.qpcRefreshPeriod;
    ULONGLONG targetQpc=targetRefreshQpc+safetyTicks;
    if(targetQpc<=static_cast<ULONGLONG>(now.QuadPart)) return false;

    long double remainingHundredNanoseconds=
        static_cast<long double>(targetQpc-now.QuadPart)*10000000.0L
            /performanceCounterFrequency.QuadPart;
    LARGE_INTEGER dueTime{};
    dueTime.QuadPart=-max<LONGLONG>(
        1,
        static_cast<LONGLONG>(ceil(remainingHundredNanoseconds))
    );
    return SetWaitableTimer(
        compositionFrameTimer,&dueTime,0,nullptr,nullptr,FALSE
    )!=FALSE;
}
void waitForKeyRelease(int virtualKey)
{
    while(GetAsyncKeyState(virtualKey)&0x8000)
    {
        preciseWait(1);
    }
}

void captureRegion(const ScreenRect& region)
{
    BitBlt(
        hMemoryDC,
        region.left-x,
        region.top-y,
        region.width(),
        region.height(),
        hScreenDC,
        region.left,
        region.top,
        SRCCOPY
    );
    pixels=static_cast<unsigned int*>(pPixels);
}
void capture()
{
    captureRegion({x,y,x+width,y+height});
}
void captureQueue()
{
    captureRegion(screenLayout.nextQueue);
}
void captureBoard()
{
    captureRegion(screenLayout.board);
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
    compositionFrameTimer=CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );
    if(!compositionFrameTimer)
    {
        compositionFrameTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    }
    QueryPerformanceFrequency(&performanceCounterFrequency);
}
void cleanupCapture()
{
    if(compositionFrameTimer)
    {
        CancelWaitableTimer(compositionFrameTimer);
        CloseHandle(compositionFrameTimer);
        compositionFrameTimer=nullptr;
    }
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
double getPieceColorDistance(color first,color second)
{
    // Compare chromaticity instead of absolute RGB intensity. Piece previews
    // and locked blocks use the same hue at different brightness levels, so a
    // raw RGB distance can confuse a dark yellow O with an orange L.
    double firstTotal=max(1,first.R+first.G+first.B);
    double secondTotal=max(1,second.R+second.G+second.B);
    double red=first.R/firstTotal-second.R/secondTotal;
    double green=first.G/firstTotal-second.G/secondTotal;
    double blue=first.B/firstTotal-second.B/secondTotal;
    return red*red+green*green+blue*blue;
}
double getNearestPieceColorDistance(color sampledColor)
{
    double bestDistance=numeric_limits<double>::max();
    for(const Piece& piece:all_p)
    {
        bestDistance=min(bestDistance,getPieceColorDistance(sampledColor,piece.c));
    }
    return bestDistance;
}
color getRepresentativePieceColor(int tx,int ty,int bx,int by)
{
    color bestColor(0,0,0);
    double bestDistance=numeric_limits<double>::max();
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

            double distance=getNearestPieceColorDistance(sampledColor);
            if(distance<bestDistance)
            {
                bestDistance=distance;
                bestColor=sampledColor;
            }
        }
    }
    return bestColor;
}
vector<color>getQueueColors(int slotCount=queueLength)
{
    slotCount=clamp(slotCount,1,queueLength);
    vector<color>ret;
    ret.reserve(slotCount);
    int tx=screenLayout.nextQueue.left-x;
    int bx=screenLayout.nextQueue.right-x-1;
    int queueTop=screenLayout.nextQueue.top-y;
    int queueHeight=screenLayout.nextQueue.height();
    for(int i=0;i<slotCount;i++)
    {
        int ty=queueTop+(queueHeight*i)/queueLength;
        int by=queueTop+(queueHeight*(i+1))/queueLength-1;
        int verticalMargin=max(1,(by-ty+1)/queueSlotVerticalMarginDivisor);
        ty+=verticalMargin;
        by-=verticalMargin;
        ret.push_back(getRepresentativePieceColor(tx,ty,bx,by));
    }
    return ret;
}
Piece getPieceByColor(color c)
{
    double mx=numeric_limits<double>::max();
    int mxi=0;
    for(int i=0;i<7;i++)
    {
        double diff=getPieceColorDistance(c,all_p[i].c);
        if(diff<mx)
        {
            mx=diff,mxi=i;
        }
    }
    return all_p[mxi];
}
vector<Piece>getQueue(
    vector<color>* sampledColors=nullptr,
    int slotCount=queueLength
)
{
    vector<Piece>ret;
    vector<color>colors=getQueueColors(slotCount);
    ret.reserve(colors.size());
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
bool isReliableQueueReading(
    const vector<Piece>& queue,
    const vector<color>& sampledColors
)
{
    if(queue.empty() || queue.size()>queueLength
        || queue.size()!=sampledColors.size())
    {
        return false;
    }
    array<int,7>pieceCounts{};
    for(size_t index=0;index<sampledColors.size();index++)
    {
        const color& sampled=sampledColors[index];
        int highest=max({sampled.R,sampled.G,sampled.B});
        int lowest=min({sampled.R,sampled.G,sampled.B});
        if(highest<50 || highest-lowest<20
            || getNearestPieceColorDistance(sampled)>maximumQueueColorDistance)
        {
            return false;
        }
        pieceCounts[queue[index].ind]++;
    }
    // Any five consecutive pieces from two seven-bags can contain a piece at
    // most twice. This rejects transition artifacts such as JJJJJ.
    return *max_element(pieceCounts.begin(),pieceCounts.end())<=2;
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
bool queueStartsWith(const vector<Piece>& queue,const vector<Piece>& prefix)
{
    if(prefix.size()>queue.size()) return false;
    for(size_t index=0;index<prefix.size();index++)
    {
        if(queue[index].type!=prefix[index].type) return false;
    }
    return true;
}
bool queuesHaveSameTypes(const vector<Piece>& first,const vector<Piece>& second)
{
    if(first.size()!=second.size()) return false;
    for(size_t index=0;index<first.size();index++)
    {
        if(first[index].type!=second[index].type) return false;
    }
    return true;
}
bool isValidSevenBagPrefix(const vector<int>& sequence)
{
    for(size_t bagStart=0;bagStart<sequence.size();bagStart+=7)
    {
        array<bool,7>seen{};
        size_t bagEnd=min(sequence.size(),bagStart+7);
        for(size_t index=bagStart;index<bagEnd;index++)
        {
            int pieceIndex=sequence[index];
            if(pieceIndex<0 || pieceIndex>=7 || seen[pieceIndex]) return false;
            seen[pieceIndex]=true;
        }
    }
    return true;
}
bool isValidQueueExtension(
    const vector<int>& knownSequence,
    const vector<Piece>& capturedQueue,
    size_t overlapSize
)
{
    if(overlapSize>capturedQueue.size()) return false;
    vector<int>extendedSequence=knownSequence;
    extendedSequence.reserve(
        knownSequence.size()+capturedQueue.size()-overlapSize
    );
    for(size_t index=overlapSize;index<capturedQueue.size();index++)
    {
        extendedSequence.push_back(capturedQueue[index].ind);
    }
    return isValidSevenBagPrefix(extendedSequence);
}
void appendQueueExtension(
    vector<int>& knownSequence,
    const vector<Piece>& capturedQueue,
    size_t overlapSize
)
{
    for(size_t index=overlapSize;index<capturedQueue.size();index++)
    {
        knownSequence.push_back(capturedQueue[index].ind);
    }
}
struct UpdatedFrame {
    vector<Piece> queue;
    vector<color> queueColors;
    double renderWaitMs=0;
    double captureMs=0;
    double queueReadMs=0;
    double initialWaitMs=0;
    double retryWaitMs=0;
    double captureCpuMs=0;
    double captureOffCpuMs=0;
    double synchronizationMs=0;
    double otherSynchronizationMs=0;
    bool captureCpuMeasured=false;
    int captureAttempts=0;
    int unreliableFrames=0;
    int unchangedFrames=0;
    int overlapMismatchFrames=0;
    int sevenBagMismatchFrames=0;
};

double currentThreadCpuMilliseconds()
{
    FILETIME creationTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if(!GetThreadTimes(
        GetCurrentThread(),
        &creationTime,
        &exitTime,
        &kernelTime,
        &userTime
    ))
    {
        return numeric_limits<double>::quiet_NaN();
    }
    ULARGE_INTEGER kernel{};
    kernel.LowPart=kernelTime.dwLowDateTime;
    kernel.HighPart=kernelTime.dwHighDateTime;
    ULARGE_INTEGER user{};
    user.LowPart=userTime.dwLowDateTime;
    user.HighPart=userTime.dwHighDateTime;
    return static_cast<double>(kernel.QuadPart+user.QuadPart)/10000.0;
}

UpdatedFrame waitForQueueRefresh(
    const vector<Piece>& expectedPrefix,
    const vector<Piece>& previousCapturedQueue,
    const vector<int>& knownSequence,
    DWORD initialWaitMilliseconds=queueBatchSettleDelayMs,
    bool waitForCompositionFrame=false
)
{
    UpdatedFrame result;
    bool lastReadingReliable=false;
    auto synchronizationStart=chrono::steady_clock::now();
    benchmarkStats.activeStage="queue refresh synchronization";
    benchmarkStats.activeStageStart=synchronizationStart;

    while(true)
    {
        auto waitStart=chrono::steady_clock::now();
        if(result.captureAttempts==0 && waitForCompositionFrame)
        {
            DWORD waitResult=WaitForSingleObject(
                compositionFrameTimer,
                screenUpdateTimeoutMs
            );
            if(waitResult!=WAIT_OBJECT_0)
            {
                throw runtime_error(
                    "DWM composition-frame timer did not signal"
                );
            }
        }
        else
        {
            preciseWait(
                result.captureAttempts==0
                    ?initialWaitMilliseconds
                    :screenUpdateDelayMs
            );
        }
        double waitMs=chrono::duration<double,milli>(
            chrono::steady_clock::now()-waitStart
        ).count();
        result.renderWaitMs+=waitMs;
        if(result.captureAttempts==0) result.initialWaitMs+=waitMs;
        else result.retryWaitMs+=waitMs;

        double captureCpuStart=currentThreadCpuMilliseconds();
        auto captureStart=chrono::steady_clock::now();
        captureQueue();
        double captureWallMs=chrono::duration<double,milli>(
            chrono::steady_clock::now()-captureStart
        ).count();
        double captureCpuEnd=currentThreadCpuMilliseconds();
        result.captureMs+=captureWallMs;
        if(isfinite(captureCpuStart) && isfinite(captureCpuEnd))
        {
            double captureCpuMs=max(0.0,captureCpuEnd-captureCpuStart);
            result.captureCpuMs+=captureCpuMs;
            result.captureOffCpuMs+=max(0.0,captureWallMs-captureCpuMs);
            result.captureCpuMeasured=true;
        }
        result.captureAttempts++;

        auto queueStart=chrono::steady_clock::now();
        result.queue=getQueue(&result.queueColors);
        result.queueReadMs+=chrono::duration<double,milli>(
            chrono::steady_clock::now()-queueStart
        ).count();

        lastReadingReliable=isReliableQueueReading(
            result.queue,result.queueColors
        );
        bool changed=!queuesHaveSameTypes(previousCapturedQueue,result.queue);
        bool validBagExtension=isValidQueueExtension(
            knownSequence,
            result.queue,
            expectedPrefix.size()
        );
        bool overlapMatches=queueStartsWith(result.queue,expectedPrefix);
        if(lastReadingReliable && changed && overlapMatches && validBagExtension)
        {
            result.synchronizationMs=chrono::duration<double,milli>(
                chrono::steady_clock::now()-synchronizationStart
            ).count();
            result.otherSynchronizationMs=max(
                0.0,
                result.synchronizationMs-result.renderWaitMs
                    -result.captureMs-result.queueReadMs
            );
            benchmarkStats.activeStage.clear();
            return result;
        }
        if(!lastReadingReliable) result.unreliableFrames++;
        else if(!changed) result.unchangedFrames++;
        else if(!overlapMatches) result.overlapMismatchFrames++;
        else result.sevenBagMismatchFrames++;

        double elapsed=chrono::duration<double,milli>(
            chrono::steady_clock::now()-synchronizationStart
        ).count();
        if(elapsed>=screenUpdateTimeoutMs)
        {
            ostringstream error;
            error<<"Timed out refreshing NEXT queue after "<<elapsed
                 <<" ms; expected prefix="<<getQueueTypes(expectedPrefix)
                 <<", previous capture="<<getQueueTypes(previousCapturedQueue)
                 <<", last seen="<<getQueueTypes(result.queue)
                 <<", reliable="<<(lastReadingReliable?"yes":"no")
                 <<", seven-bag extension="
                 <<(validBagExtension?"yes":"no")
                 <<", rejected frames: unreliable="
                 <<result.unreliableFrames
                 <<", unchanged="<<result.unchangedFrames
                 <<", overlap mismatch="<<result.overlapMismatchFrames
                 <<", seven-bag mismatch="<<result.sevenBagMismatchFrames;
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
vector<int>firstInCol(10,0);
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
int countBoardDifferences(
    const vector<vector<int>>& expected,
    int firstComparedRow=4
)
{
    int differences=0;
    for(int row=firstComparedRow;row<boardRows;row++)
    {
        for(int column=0;column<boardColumns;column++)
        {
            differences+=expected[row][column]!=grid[row][column];
        }
    }
    return differences;
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
    for(const auto& cell:p.cells[rot])
    {
        int row=to_put+cell.second;
        if(row<0 || row>=boardRows)
        {
            return -1;
        }
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
    optional<char> currentPieceType=nullopt,
    bool saveCapturedImage=true,
    const string& boardSource="captured image"
)
{
    filesystem::path capturePath;
    if(saveCapturedImage) capturePath=saveDebugCapture();
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
    cout<<"Board source: "<<boardSource<<'\n';
    if(currentPieceType)
    {
        cout<<"Tracked current piece: "<<*currentPieceType<<'\n';
    }

    cout<<"\nBoard state (# = filled, . = empty)\n";
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
            <<"  chromaticity distance="
            <<getPieceColorDistance(sampled,queue[i].c)<<'\n';
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

// Lower scores are better. These features favor a low, smooth, accessible
// surface and make creating or burying holes substantially more expensive
// than clearing a line.
struct BoardFeatures {
    int aggregateHeight=0;
    int maximumHeight=0;
    int holes=0;
    int holeDepth=0;
    int bumpiness=0;
    int wells=0;
    int rowTransitions=0;
    int columnTransitions=0;
};

BoardFeatures analyzeGrid()
{
    BoardFeatures features;
    array<int,boardColumns> heights{};

    for(int column=0;column<boardColumns;column++)
    {
        bool blockSeen=false;
        int blocksAbove=0;
        for(int row=0;row<boardRows;row++)
        {
            if(grid[row][column])
            {
                if(!blockSeen)
                {
                    heights[column]=boardRows-row;
                    blockSeen=true;
                }
                blocksAbove++;
            }
            else if(blockSeen)
            {
                features.holes++;
                features.holeDepth+=blocksAbove;
            }
        }
        features.aggregateHeight+=heights[column];
        features.maximumHeight=max(features.maximumHeight,heights[column]);
    }

    for(int column=0;column+1<boardColumns;column++)
    {
        features.bumpiness+=abs(heights[column]-heights[column+1]);
    }

    for(int row=0;row<boardRows;row++)
    {
        int previous=1; // The side walls count as occupied.
        for(int column=0;column<boardColumns;column++)
        {
            int occupied=grid[row][column]!=0;
            features.rowTransitions+=occupied!=previous;
            previous=occupied;
        }
        features.rowTransitions+=previous!=1;
    }

    for(int column=0;column<boardColumns;column++)
    {
        int previous=1; // The ceiling and floor count as occupied boundaries.
        for(int row=0;row<boardRows;row++)
        {
            int occupied=grid[row][column]!=0;
            features.columnTransitions+=occupied!=previous;
            previous=occupied;
        }
        features.columnTransitions+=previous!=1;
    }

    for(int column=0;column<boardColumns;column++)
    {
        int wellDepth=0;
        for(int row=0;row<boardRows;row++)
        {
            bool leftFilled=column==0 || grid[row][column-1];
            bool rightFilled=column==boardColumns-1 || grid[row][column+1];
            if(!grid[row][column] && leftFilled && rightFilled)
            {
                wellDepth++;
                features.wells+=wellDepth;
            }
            else
            {
                wellDepth=0;
            }
        }
    }
    return features;
}

int getScoreFromFeatures(const BoardFeatures& features)
{
    int dangerHeight=max(0,features.maximumHeight-12);
    return
        features.aggregateHeight*40
        +features.maximumHeight*75
        +features.bumpiness*25
        +features.holes*1000
        +features.holeDepth*250
        +features.holes*features.holes*400
        +features.wells*20
        +features.rowTransitions*10
        +features.columnTransitions*10
        +dangerHeight*dangerHeight*100;
}

int getScoreOfGrid()
{
    BoardFeatures features=analyzeGrid();
    return getScoreFromFeatures(features);
}

int getLineClearReward(int clearedLines)
{
    static constexpr array<int,5> rewards{0,100,250,450,700};
    return rewards[clamp(clearedLines,0,4)];
}

int getImmediatePlacementPenalty()
{
    BoardFeatures features=analyzeGrid();
    return features.holes*20000+features.holeDepth*1000;
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
vector<vector<int>> predictBoardAfterPlacement(
    const Piece& piece,
    int position,
    int rotation
)
{
    saveGrid();
    int landingRow=getLowestRow(position,rotation,piece);
    pushPiece(landingRow,position,rotation,piece);
    clear_all_grid();
    vector<vector<int>> predicted=grid;
    resetGrid();
    unsaveGrid();
    return predicted;
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

// Search uses a compact copy of the board. Each row is a 10-bit mask, so a
// recursive branch can be copied without allocating or touching the live grid.
struct CompactBoard {
    array<uint16_t,boardRows> rows{};
    array<uint8_t,boardColumns> heights{};
};

CompactBoard makeCompactBoard()
{
    CompactBoard board;
    for(int row=0;row<boardRows;row++)
    {
        uint16_t mask=0;
        for(int column=0;column<boardColumns;column++)
        {
            if(grid[row][column]) mask|=uint16_t{1}<<column;
        }
        board.rows[row]=mask;
    }
    for(int column=0;column<boardColumns;column++)
    {
        board.heights[column]=static_cast<uint8_t>(firstInCol[column]);
    }
    return board;
}

int getLowestRow(const CompactBoard& board,int position,int rotation,const Piece& piece)
{
    int landingRow=boardRows;
    for(const auto& cell:piece.cells[rotation])
    {
        int column=position+cell.first;
        if(column<0 || column>=boardColumns) return -1;
        landingRow=min(
            landingRow,
            boardRows-1-static_cast<int>(board.heights[column])-cell.second
        );
    }
    for(const auto& cell:piece.cells[rotation])
    {
        int row=landingRow+cell.second;
        if(row<0 || row>=boardRows) return -1;
    }
    return landingRow;
}

void rebuildCompactHeights(CompactBoard& board)
{
    board.heights.fill(0);
    for(int row=0;row<boardRows;row++)
    {
        uint16_t cells=board.rows[row];
        if(cells==0) continue;
        for(int column=0;column<boardColumns;column++)
        {
            if(board.heights[column]==0 && (cells&(uint16_t{1}<<column)))
            {
                board.heights[column]=static_cast<uint8_t>(boardRows-row);
            }
        }
    }
}

int placeAndClear(
    CompactBoard& board,
    int landingRow,
    int position,
    int rotation,
    const Piece& piece
)
{
    for(const auto& cell:piece.cells[rotation])
    {
        int row=landingRow+cell.second;
        int column=position+cell.first;
        if(row<0 || row>=boardRows || column<0 || column>=boardColumns)
        {
            throw runtime_error("Invalid compact-board placement");
        }
        uint16_t cellMask=uint16_t{1}<<column;
        if(board.rows[row]&cellMask)
        {
            throw runtime_error("Invalid compact-board placement");
        }
        board.rows[row]|=cellMask;
        board.heights[column]=static_cast<uint8_t>(max(
            static_cast<int>(board.heights[column]),boardRows-row
        ));
    }

    constexpr uint16_t fullRowMask=(uint16_t{1}<<boardColumns)-1;
    int destination=boardRows-1;
    int clearedLines=0;
    for(int source=boardRows-1;source>=0;source--)
    {
        if(board.rows[source]==fullRowMask)
        {
            clearedLines++;
        }
        else
        {
            board.rows[destination--]=board.rows[source];
        }
    }
    while(destination>=0) board.rows[destination--]=0;
    if(clearedLines>0) rebuildCompactHeights(board);
    return clearedLines;
}

BoardFeatures analyzeGrid(const CompactBoard& board)
{
    BoardFeatures features;
    for(int column=0;column<boardColumns;column++)
    {
        int height=board.heights[column];
        features.aggregateHeight+=height;
        features.maximumHeight=max(features.maximumHeight,height);

        bool blockSeen=false;
        int blocksAbove=0;
        int previous=1;
        for(int row=0;row<boardRows;row++)
        {
            int occupied=(board.rows[row]>>column)&1;
            features.columnTransitions+=occupied!=previous;
            previous=occupied;
            if(occupied)
            {
                blockSeen=true;
                blocksAbove++;
            }
            else if(blockSeen)
            {
                features.holes++;
                features.holeDepth+=blocksAbove;
            }
        }
        features.columnTransitions+=previous!=1;
    }

    for(int column=0;column+1<boardColumns;column++)
    {
        features.bumpiness+=abs(
            static_cast<int>(board.heights[column])
            -static_cast<int>(board.heights[column+1])
        );
    }

    for(int row=0;row<boardRows;row++)
    {
        int previous=1;
        for(int column=0;column<boardColumns;column++)
        {
            int occupied=(board.rows[row]>>column)&1;
            features.rowTransitions+=occupied!=previous;
            previous=occupied;
        }
        features.rowTransitions+=previous!=1;
    }

    array<int,boardColumns> wellDepth{};
    for(int row=0;row<boardRows;row++)
    {
        for(int column=0;column<boardColumns;column++)
        {
            bool occupied=(board.rows[row]>>column)&1;
            bool leftFilled=column==0 || ((board.rows[row]>>(column-1))&1);
            bool rightFilled=column==boardColumns-1
                || ((board.rows[row]>>(column+1))&1);
            if(!occupied && leftFilled && rightFilled)
            {
                features.wells+=++wellDepth[column];
            }
            else
            {
                wellDepth[column]=0;
            }
        }
    }
    return features;
}

struct CompactSearchResult {
    int position=4;
    int rotation=0;
    int score=10000000;
};

CompactSearchResult getBestPosCompact(
    const CompactBoard& board,
    const Piece& piece,
    int depth
)
{
    CompactSearchResult best;
    for(int rotation=0;rotation<static_cast<int>(piece.cells.size());rotation++)
    {
        for(int position=-1;position<boardColumns;position++)
        {
            int landingRow=getLowestRow(board,position,rotation,piece);
            if(landingRow<0) continue;

            CompactBoard next=board;
            int clearedLines=placeAndClear(
                next,landingRow,position,rotation,piece
            );
            BoardFeatures features=analyzeGrid(next);
            int futureScore=depth>=maxDepth
                ?getScoreFromFeatures(features)
                :getBestPosCompact(next,curQueue[depth],depth+1).score;
            int score=futureScore
                +features.holes*20000
                +features.holeDepth*1000
                -getLineClearReward(clearedLines);

            if(depth==0) retV.push_back({position,rotation,score});
            if(score<best.score)
            {
                best={position,rotation,score};
            }
        }
    }
    return best;
}

CompactSearchResult getBestPosCompactParallel(
    const CompactBoard& board,
    const Piece& piece
)
{
    vector<CompactSearchResult> candidates;
    for(int rotation=0;rotation<static_cast<int>(piece.cells.size());rotation++)
    {
        for(int position=-1;position<boardColumns;position++)
        {
            if(getLowestRow(board,position,rotation,piece)>=0)
            {
                candidates.push_back({position,rotation,10000000});
            }
        }
    }
    if(candidates.empty()) return {};

    unsigned int available=thread::hardware_concurrency();
    int requested=compactSearchThreadCount>0
        ?compactSearchThreadCount
        :static_cast<int>(available==0?1:available);
    int workerCount=max(1,min(static_cast<int>(candidates.size()),requested));
    if(workerCount==1)
    {
        return getBestPosCompact(board,piece,0);
    }

    atomic<size_t> nextCandidate{0};
    atomic<bool> cancelWorkers{false};
    mutex errorMutex;
    exception_ptr workerError;
    auto evaluateCandidate=[&]() {
        while(!cancelWorkers.load(memory_order_relaxed))
        {
            size_t index=nextCandidate.fetch_add(1,memory_order_relaxed);
            if(index>=candidates.size()) return;
            try
            {
                CompactSearchResult& candidate=candidates[index];
                int landingRow=getLowestRow(
                    board,candidate.position,candidate.rotation,piece
                );
                CompactBoard next=board;
                int clearedLines=placeAndClear(
                    next,landingRow,candidate.position,candidate.rotation,piece
                );
                BoardFeatures features=analyzeGrid(next);
                int futureScore=maxDepth==0
                    ?getScoreFromFeatures(features)
                    :getBestPosCompact(next,curQueue[0],1).score;
                candidate.score=futureScore
                    +features.holes*20000
                    +features.holeDepth*1000
                    -getLineClearReward(clearedLines);
            }
            catch(...)
            {
                {
                    lock_guard<mutex> errorLock(errorMutex);
                    if(!workerError) workerError=current_exception();
                }
                cancelWorkers.store(true,memory_order_relaxed);
                return;
            }
        }
    };

    vector<thread> workers;
    workers.reserve(workerCount);
    for(int index=0;index<workerCount;index++) workers.emplace_back(evaluateCandidate);
    for(thread& worker:workers) worker.join();
    if(workerError) rethrow_exception(workerError);

    CompactSearchResult best;
    for(const CompactSearchResult& candidate:candidates)
    {
        retV.push_back({candidate.position,candidate.rotation,candidate.score});
        if(candidate.score<best.score) best=candidate;
    }
    return best;
}

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
            int placementPenalty=getImmediatePlacementPenalty();
            array<int,3>cur=getBestPos(curQueue[curDepth],curDepth+1);
            int curScore=cur[2]+placementPenalty-getLineClearReward(cst);
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
    CompactBoard board=makeCompactBoard();
    CompactSearchResult best=maxDepth>=parallelSearchDepthThreshold
        ?getBestPosCompactParallel(board,p)
        :getBestPosCompact(board,p,0);
    return {best.position,best.rotation,best.score};
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
            total_clr_cost+=getImmediatePlacementPenalty();
            total_clr_cost-=getLineClearReward(cst);
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
        else if(argument=="--lookahead")
        {
            if(i+1>=argc)
            {
                cerr<<"--lookahead requires a value from 0 to "
                    <<queueLength-1<<endl;
                return 1;
            }
            string value=argv[++i];
            size_t parsed=0;
            try
            {
                realMaxDepth=stoi(value,&parsed);
            }
            catch(const exception&)
            {
                parsed=0;
                realMaxDepth=-1;
            }
            if(parsed!=value.size() || realMaxDepth<0 || realMaxDepth>=queueLength)
            {
                cerr<<"--lookahead must be an integer from 0 to "
                    <<queueLength-1<<endl;
                return 1;
            }
        }
        else if(argument=="--help")
        {
            cout<<"Usage: color.exe [--debug | --inspect] [--lookahead N]\n"
                <<"  --debug    Run the bot and log every tracked state.\n"
                <<"  --inspect  Capture and log one state without playing.\n"
                <<"  --lookahead N  Search 0 to "<<queueLength-1
                <<" queued pieces (default: "<<queuedPiecesToLookAhead<<").\n";
            return 0;
        }
        else
        {
            cerr<<"Unknown option: "<<argument<<"\nUse --help to list options."<<endl;
            return 1;
        }
    }
    queueRefreshBatchSize=min(
        queueLength-1,
        queueLength+1-realMaxDepth
    );
    cout<<"Solver lookahead: "<<realMaxDepth<<" queued piece(s)\n"
        <<"NEXT capture interval: every "<<queueRefreshBatchSize
        <<" hard drop(s)"<<endl;
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
    cout<<(inspectOnly
        ?"Press P when the game is visible and ready."
        :"Press P while the opening five-piece queue is visible, before the game starts.")
        <<endl;
    while (true) {
        if (GetAsyncKeyState(0x50) & 0x8000) {
            cout<<(inspectOnly?"Capturing inspection frame...":"Capturing opening queue...")<<endl;
            cout.flush();
            break;
        }
        Sleep(inputPollingDelayMs);
    }
    vector<color>queueColors;
    if(inspectOnly)
    {
        capture();
        load_grid();
        curQueue=getQueue(&queueColors);
        printDebugSnapshot("initial capture",curQueue,queueColors);
        cleanupCapture();
        return 0;
    }
    captureQueue();
    curQueue=getQueue(&queueColors);
    if(!isReliableQueueReading(curQueue,queueColors))
    {
        throw runtime_error(
            "The opening queue capture is incomplete or has unreliable colors"
        );
    }
    vector<int>knownPieceSequence;
    knownPieceSequence.reserve(queueLength);
    for(const Piece& piece:curQueue) knownPieceSequence.push_back(piece.ind);
    if(!isValidSevenBagPrefix(knownPieceSequence))
    {
        throw runtime_error(
            "The opening queue is not a valid prefix of a fresh seven-bag"
        );
    }
    if(debugMode)
    {
        printDebugSnapshot(
            "initial NEXT capture",
            curQueue,
            queueColors,
            curQueue.front().type,
            false,
            "known empty board"
        );
    }
    Piece manuallyPlayedPiece=curQueue[0];
    Piece firstBotPiece=curQueue[1];
    vector<Piece>openingQueue=curQueue;
    // Three slots make the one-time opening transition unambiguous even when
    // the first two pieces happen to repeat across a seven-bag boundary.
    size_t openingKnownSlots=min<size_t>(3,openingQueue.size()-2);
    vector<Piece>expectedQueuePrefix(
        openingQueue.begin()+2,
        openingQueue.begin()+2+openingKnownSlots
    );
    cout<<"Opening queue: "<<getQueueTypes(openingQueue)<<'\n'
        <<"Start the game, then hard-drop "<<manuallyPlayedPiece.type
        <<" at its default position and orientation."<<endl;
    while (true) {
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
            // Do not let the first injected hard drop overlap the user's
            // physical Space press. TETR.IO needs a fresh press edge.
            waitForKeyRelease(VK_SPACE);
            break;
        }
        Sleep(inputPollingDelayMs);
    }
    UpdatedFrame firstUpdatedFrame=waitForQueueRefresh(
        expectedQueuePrefix,
        openingQueue,
        knownPieceSequence
    );
    appendQueueExtension(
        knownPieceSequence,
        firstUpdatedFrame.queue,
        expectedQueuePrefix.size()
    );
    vector<vector<int>>openingBoard=predictBoardAfterPlacement(
        manuallyPlayedPiece,4,0
    );
    grid=openingBoard;
    clear_all_grid();
    curPiece=firstBotPiece;
    curQueue=move(firstUpdatedFrame.queue);
    queueColors=move(firstUpdatedFrame.queueColors);
    vector<Piece>lastCapturedQueue=curQueue;
    int movesSinceQueueCapture=0;
    chrono::steady_clock::time_point firstInputInBatch;
    bool compositionFrameTimerArmed=false;
    double batchCompositionRefreshIntervalMs=0;
    if(debugMode)
    {
        printDebugSnapshot(
            "after known manual opening placement",
            curQueue,
            queueColors,
            curPiece.type,
            false,
            "known empty board plus default opening hard drop"
        );
    }
    auto takeoverStart=chrono::steady_clock::now();
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
        vector<vector<int>>predictedBoard=predictBoardAfterPlacement(
            curPiece,
            best_play[0],
            best_play[1]
        );
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
            cout<<"score after chosen move: "<<best_play[2]<<endl;
        }
        
        
        



        //2
        benchmarkStats.beginStage("placement input");
        size_t keyPressCount=actuallyPutThePiece(best_play[0],best_play[1]);
        double placementInputMs=benchmarkStats.finishStage(benchmarkStats.placementInput);
        if(movesSinceQueueCapture==0)
        {
            firstInputInBatch=chrono::steady_clock::now();
            compositionFrameTimerArmed=armCompositionFrameTimer(
                max(1,queueRefreshBatchSize-1),
                batchCompositionRefreshIntervalMs
            );
        }

        auto boardUpdateStart=chrono::steady_clock::now();
        grid=move(predictedBoard);
        clear_all_grid();
        double gridReadMs=chrono::duration<double,milli>(
            chrono::steady_clock::now()-boardUpdateStart
        ).count();

        if(curQueue.empty())
        {
            throw runtime_error("The local NEXT queue ran out of pieces");
        }
        Piece nextActivePiece=curQueue.front();
        curQueue.erase(curQueue.begin());
        if(!queueColors.empty()) queueColors.erase(queueColors.begin());
        movesSinceQueueCapture++;

        UpdatedFrame updatedFrame;
        bool queueSynchronized=false;
        if(movesSinceQueueCapture>=queueRefreshBatchSize)
        {
            vector<Piece>expectedQueuePrefix=curQueue;
            DWORD scheduledWaitMs=0;
            if(!compositionFrameTimerArmed)
            {
                double elapsedSinceFirstInput=chrono::duration<double,milli>(
                    chrono::steady_clock::now()-firstInputInBatch
                ).count();
                double desiredCaptureStartMs=
                    (queueRefreshBatchSize-1)*expectedFrameIntervalMs;
                scheduledWaitMs=static_cast<DWORD>(ceil(max(
                    0.0,
                    desiredCaptureStartMs-elapsedSinceFirstInput
                )));
            }
            updatedFrame=waitForQueueRefresh(
                expectedQueuePrefix,
                lastCapturedQueue,
                knownPieceSequence,
                scheduledWaitMs,
                compositionFrameTimerArmed
            );
            appendQueueExtension(
                knownPieceSequence,
                updatedFrame.queue,
                expectedQueuePrefix.size()
            );
            curQueue=move(updatedFrame.queue);
            queueColors=move(updatedFrame.queueColors);
            lastCapturedQueue=curQueue;
            movesSinceQueueCapture=0;
            queueSynchronized=true;
            benchmarkStats.queueSynchronizationCount++;
            if(compositionFrameTimerArmed)
            {
                benchmarkStats.compositionClockSynchronizationCount++;
                benchmarkStats.compositionRefreshInterval.add(
                    batchCompositionRefreshIntervalMs
                );
            }
            compositionFrameTimerArmed=false;
            benchmarkStats.queueCaptureAttemptCount+=
                updatedFrame.captureAttempts;
            benchmarkStats.unreliableQueueFrames+=
                updatedFrame.unreliableFrames;
            benchmarkStats.unchangedQueueFrames+=
                updatedFrame.unchangedFrames;
            benchmarkStats.overlapMismatchFrames+=
                updatedFrame.overlapMismatchFrames;
            benchmarkStats.sevenBagMismatchFrames+=
                updatedFrame.sevenBagMismatchFrames;
            benchmarkStats.queueSyncTotal.add(
                updatedFrame.synchronizationMs
            );
            benchmarkStats.queueInitialWait.add(
                updatedFrame.initialWaitMs
            );
            benchmarkStats.queueRetryWait.add(
                updatedFrame.retryWaitMs
            );
            benchmarkStats.queueCaptureWall.add(
                updatedFrame.captureMs
            );
            if(updatedFrame.captureCpuMeasured)
            {
                benchmarkStats.queueCaptureCpu.add(
                    updatedFrame.captureCpuMs
                );
                benchmarkStats.queueCaptureOffCpu.add(
                    updatedFrame.captureOffCpuMs
                );
            }
            benchmarkStats.queueDecodePhysical.add(
                updatedFrame.queueReadMs
            );
            benchmarkStats.queueSyncOther.add(
                updatedFrame.otherSynchronizationMs
            );
        }
        else
        {
            // Let TETR.IO's input thread run without waiting for a rendered
            // frame. The full five-millisecond settle is only needed before
            // a physical queue refresh at the end of the batch.
            auto renderWaitStart=chrono::steady_clock::now();
            Sleep(0);
            updatedFrame.renderWaitMs=chrono::duration<double,milli>(
                chrono::steady_clock::now()-renderWaitStart
            ).count();
        }
        curPiece=nextActivePiece;
        bool boardResynchronized=false;

        auto timeSinceTakeover=chrono::duration_cast<chrono::milliseconds>(
            chrono::steady_clock::now()-takeoverStart
        ).count();
        if(timeSinceTakeover>=boardResyncWarmupMs
            && moveNumber%boardResyncIntervalMoves==0)
        {
            vector<vector<int>>simulatedBoard=grid;
            auto boardCaptureStart=chrono::steady_clock::now();
            captureBoard();
            updatedFrame.captureMs+=chrono::duration<double,milli>(
                chrono::steady_clock::now()-boardCaptureStart
            ).count();

            benchmarkStats.activeStage="board resynchronization";
            benchmarkStats.activeStageStart=chrono::steady_clock::now();
            load_grid();
            // The active piece may be visible in the spawn rows. Those cells
            // are not part of the locked board.
            for(int row=0;row<4;row++) grid[row]=simulatedBoard[row];
            clear_all_grid();
            gridReadMs+=benchmarkStats.activeStageElapsed();
            benchmarkStats.activeStage.clear();

            int boardDifferences=countBoardDifferences(simulatedBoard);
            benchmarkStats.boardResyncCount++;
            boardResynchronized=true;
            if(boardDifferences>0)
            {
                benchmarkStats.boardDriftCorrectionCount++;
                benchmarkStats.correctedBoardCells+=boardDifferences;
                cerr<<"Board state corrected on move "<<moveNumber
                    <<" ("<<boardDifferences<<" differing cells).\n";
            }
        }
        double renderWaitMs=updatedFrame.renderWaitMs;
        double captureMs=updatedFrame.captureMs;
        double queueReadMs=updatedFrame.queueReadMs;
        benchmarkStats.renderWait.add(renderWaitMs);
        benchmarkStats.screenCapture.add(captureMs);
        benchmarkStats.gridRead.add(gridReadMs);
        benchmarkStats.queueRead.add(queueReadMs);
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
                curPiece.type,
                boardResynchronized,
                boardResynchronized
                    ?"periodic physical board resynchronization"
                    :"trusted simulation since the last resynchronization"
            );
        }
        double cycleMs=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-cycleStart
        ).count();
        benchmarkStats.fullCycle.add(cycleMs);
        if(queueSynchronized) benchmarkStats.queueSyncCycle.add(cycleMs);
        else benchmarkStats.localOnlyCycle.add(cycleMs);
        bool printMoveStatus=debugMode || moveNumber==1
            || moveNumber%statusPrintIntervalMoves==0;
        if(printMoveStatus)
        {
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
                <<"  board update: "<<gridReadMs<<" ms"
                <<"  queue: "<<queueReadMs<<" ms"
                <<"  capture attempts: "<<updatedFrame.captureAttempts
                <<"  queue synchronized: "<<(queueSynchronized?"yes":"no")
                <<"  board resynced: "<<(boardResynchronized?"yes":"no")<<"\n";
            cout<<"full cycle: "<<cycleMs<<" ms"
                <<" ("<<(1000.0/cycleMs)<<" pieces/s)"<<endl;
        }
    }
    benchmarkStats.printSummary("stopped by user");
    cleanupCapture();

    return 0;
}

#ifndef TETR_SOLVER_LIBRARY
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
#endif
