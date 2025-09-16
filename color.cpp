#include <windows.h>
#include <bits/stdc++.h>
#include <chrono>
#include <thread>
using namespace std;

/*
TODO list:
runtime optimization ideas:
represent board as a bitset, do all operations using bitwise operations
Since the depth is constant (not like chess), I can generate all possible moves for the queue, and iterate on them making them all at once
//estimation of complexity:
each piece has 10 possible positions, and an average of 3~ rotations
30^depth
when depth is 4 (current piece + 3 from queue), then i'd need 810000 (1e6)
it should be much less
actually let me calculate it rq
number of possible moves for each piece:
17 34 34 9 17 17 34 
avg = 23.33~

expected number of possible moves for each depth:
23.1429
535.592
12395.1
286859
6.63873e+06
1.53639e+08

max is 34^depth (if T,J,L and one of them gets repeated, its not that uncommon to happen..)
34^4 =~ 1e6..


current goal is to reach depth 3 in 0.1~s avg.




~~more~~ fix heuristics for the board eval
brute force on more than one move ahead (iterative deepining)

non added mechanics:
hold piece ability
side putting
spins
*/
HDC hScreenDC;
HDC hMemoryDC;
BITMAPINFO bmi;
HBITMAP hBitmap;

int maxDepth=1;
int realMaxDepth=0;
//realMaxDepth means how much lookahead in queue
int x = 785;      // top-left X
int y = 180;      // top-left Y
int width = 520;  // region width
int height = 720; // region height
void* pPixels = nullptr;
unsigned int* pixels;
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
vector<vector<int>>tempGrid;
int cell_size=35;
int grid_width=350,grid_height=700;
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

void pressKey(WORD keyCode) {
    INPUT ip;
    ip.type = INPUT_KEYBOARD;
    ip.ki.wScan = 0;
    ip.ki.time = 0;
    ip.ki.dwExtraInfo = 0;

    // Key press
    ip.ki.wVk = keyCode;     
    ip.ki.dwFlags = 0;      
    SendInput(1, &ip, sizeof(INPUT));

    // Key release
    ip.ki.dwFlags = KEYEVENTF_KEYUP; 
    SendInput(1, &ip, sizeof(INPUT));
}

void capture()
{
    BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, x, y, SRCCOPY);
    // Pixel buffer is ready in pPixels (BGRA format)
    pixels = (unsigned int*)pPixels;
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

vector<color>getQueueColors()
{
    vector<color>ret;
    for(int i=0;i<5;i++)
    {
        int ty = 50+100*i;
        int by = ty+70;
        int tx = 420;
        int bx = 420+70;
        ret.push_back(getMaxInRegion(tx,ty,bx,by));
    }
    return ret;
}
Piece getPieceByColor(color c)
{
    int mx=1000000;
    int mxi=0;
    for(int i=0;i<7;i++)
    {
        int diff=abs(c.R - all_p[i].c.R)*abs(c.R - all_p[i].c.R)+abs(c.G - all_p[i].c.G)*abs(c.G - all_p[i].c.G)+abs(c.B - all_p[i].c.B)*abs(c.B - all_p[i].c.B);
        if(diff<mx)
        {
            mx=diff,mxi=i;
        }
    }
    return all_p[mxi];
}
vector<Piece>getQueue()
{
    vector<Piece>ret;
    vector<color>colors=getQueueColors();
    for(int i=0;i<colors.size();i++)
    {
        ret.push_back(getPieceByColor(colors[i]));
    }
    return ret;

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
int firstInCol[10];
bool debugScore=0;
void load_grid()
{
    for(int i=15;i<grid_width;i+=cell_size)
    {
        for(int j=15;j<grid_height;j+=cell_size)
        {
            color cur = getMaxInRegion(i,j,i+2,j+2);
            if(cur.R>34 || cur.G>34 || cur.B>34)
            {
                grid[j/cell_size][i/cell_size] = 1;
            }
            else
            {
                grid[j/cell_size][i/cell_size] = 0;
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
    }
}
void pushPiece(int i,int j,int rot, Piece p)
{
    
    for(int k=0;k<p.cells[rot].size();k++)
    {
        int ni = i+p.cells[rot][k].second;
        int nj = j+p.cells[rot][k].first;
        assert(ni>=0 && ni<20 && nj>=0 && nj<10 && grid[ni][nj]==0);
        grid[ni][nj]=1;
    }
}
int getLowestRow(int j, int rot, Piece p)
{
    //j is the position for the "core" of the piece
    //I need to put it as down as possible
    int to_put=-1;
    ///HERE I also reduce space from top of grid..
    //this should be really optimizable
    for(int i=2;i<=20;i++)
    {
        bool valid=1;
        for(int k=0;k<p.cells[rot].size();k++)
        {
            int ni = i+p.cells[rot][k].second;
            int nj = j+p.cells[rot][k].first;
            if(ni>=20 || ni<0 || nj>=10 || nj<0 || grid[ni][nj]==1)
            {
                valid=0;
                break;
            }
        }
        if(valid)
        {
            to_put=i;
        }
        else
        {
            break;
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
void save_pic()
{
    string name = "out" + to_string(cnt++) + ".bmp";
    SaveBMP(name.c_str(),pPixels,width,height);
    cout<<"SAVED"<<endl;
}
int delayPress=8;
void actuallyPutThePiece(int pos,int rotateCount)
{
    //4 is the default position of all pieces
    //TODO, implement the ability to rotate
    //TODO, add the coordinates of rotated pieces for each piece
    int curPos=4;
    if(rotateCount == 3)
    {
        pressKey('Z');
        rotateCount=0;
        Sleep(delayPress);
    }
    if(rotateCount == 2)
    {
        pressKey('A');
        rotateCount=0;
        Sleep(delayPress);
    }
    while(rotateCount>0)
    {
        pressKey(VK_UP);
        rotateCount--;
        Sleep(delayPress);
    }
    while(curPos>pos)
    {
        pressKey(VK_LEFT);
        curPos--;
        Sleep(delayPress);
    }
    while(curPos<pos)
    {
        pressKey(VK_RIGHT);
        curPos++;
        Sleep(delayPress);
    }
    pressKey(VK_SPACE);
    //this needs to be bigger, to give time for capture
    Sleep(100);


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
    memset(firstInCol,0,sizeof(firstInCol));
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
            if(grid[i][j])
            {
                if(firstInCol[j]==0){
                    firstInCol[j]=20-i;
                }
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
        firstInCol[j]-=mn;
        firstInCol[j]++;
        sm+=(firstInCol[j] * (max(1,firstInCol[j]-5)));
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
            for(int j=0;j<10;j++)grid[i][j]=grid[rows[it]][j];
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
void saveGrid()
{
    grids.push(grid);
}
void resetGrid()
{
    grid=grids.top();
}
void unsaveGrid()
{
    grids.pop();
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
    array<int,3>best = {0,0,(int)1e9};
    array<int,3>cur;
    for(maxDepth=realMaxDepth;maxDepth<=realMaxDepth;maxDepth++)
    {
        retV.clear();
        auto start = std::chrono::high_resolution_clock::now();
        cur=getBestPos(p,0);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        cout<<maxDepth<<" "<<duration_ms.count()<<endl;
        if(cmp(best,cur)==0)best=cur;
    }
    return best;
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
        cout<<"algo2: "<<placements[i][0].first<<" "<<placements[i][0].second<<" "<<score<<endl;
        
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
int main() {
    ios_base::sync_with_stdio(0);
    cin.tie(0);
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
    preCompMoves();
   /* for(int i=0;i<7;i++)
    {
        cout<<"i: "<<i<<endl;
        for(auto j:movesForPiece[i])cout<<j.first<<" "<<j.second<<endl;
    }*/
    /*for(int j=0;j<=6;j++)cout<<dp(j)<<endl;
    
    
    cout<<endl;
    return 0;*/
    /*for(int i=0;i<20;i++)
    {
        for(int j=0;j<10;j++)
        {
            cin>>grid[i][j];
        }
    }
    cout<<getScoreOfGrid()<<endl;
    cout<<endl<<endl;
    //return 0;
    saveGrid();
    for(int j=0;j<1;j++)
    {
        for(int rot=1;rot<=1;rot++){
            //cout<<j<<" "<<rot<<":";
            if(getLowestRow(j,rot,JPiece)==-1)continue;
            pushPiece(getLowestRow(j,rot,JPiece),j,rot,JPiece);
            for(int i=0;i<20;i++)
            {
                for(int j=0;j<10;j++)cout<<grid[i][j]<<" ";
                cout<<endl;
            }
            cout<<getScoreOfGrid()<<endl;
            resetGrid();
        }
    }
    //TIL that I was redeclaring the global cur piece which was making algo 2 different than algo 1
    //2 hours for this wow...
    return 0;*/
    init();
    //I will need to use the extra space at the top later, will implement as if I don't need it tho rn
    srand(2);
    
    if (!pPixels) {
        std::cerr << "Failed to create DIB section" << std::endl;
        return -1;
    }
    cout<<"HI"<<endl;
    double milliseconds = 0;
    
    int c=0;
    //This is for starting the game
    while (true) {
        if (GetAsyncKeyState(0x50) & 0x8000) {
            cout<<"Starting game.."<<endl;;
            cout.flush();
            break;
        }
        Sleep(50); // small delay so CPU isn't 100% busy
    }
    capture(); 
    save_pic();
    load_grid();
    curQueue = getQueue();
    //I need to put a piece first before bot taking over
    while (true) {
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
            Sleep(200);
            break;
        }
        Sleep(50);
    }
    capture();    
    load_grid();
    save_pic();
    ///REMEMBER that you swapped those becaues of custom game rules
    curQueue = getQueue();
    curPiece = curQueue[0];
    /*
        Flow of logic should be something like

        1. find best place for current state
        2. put piece in that place
        3. get new statep
    */
    int cur=1;
    int cur_move=1;
    double avg_time = 0,avg_count=0;
    while (1) 
    {   
        //if I press P stop the bot (fail safe instead of ctrl c from terminal)
        if (GetAsyncKeyState(0x50) & 0x8000) {
            cout<<"done!"<<endl;
            break;
        }

        //1
        auto start = std::chrono::high_resolution_clock::now();
    
        array<int,3>best_play = getBestPosIterative(curPiece);
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        double time1 = duration_ms.count();
        for(int i=0;i<retV.size();i++)
        {
            cout<<"algo1: "<<retV[i][0]<<" "<<retV[i][1]<<" "<<retV[i][2]<<endl;
        }
        cout<<curPiece.type<<endl;
        for(int i=0;i<20;i++)
        {
            for(int j=0;j<10;j++)cout<<grid[i][j]<<" ";
            cout<<endl;
        }
        cout<<endl<<endl;


        start = std::chrono::high_resolution_clock::now();

        array<int,3>best_play2 = bruteForceOnAll();

        end = std::chrono::high_resolution_clock::now();
        duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        double time2 = duration_ms.count();


        cout<<"move:"<<cur_move++<<"\n time spent first algo:"<<time1<<" time2: "<<time2<<endl;
        cout<<curPiece.type<<endl;
        for(int i=0;i<20;i++)
        {
            for(int j=0;j<10;j++)cout<<grid[i][j]<<" ";
            cout<<endl;
        }
        cout<<endl;
        cout<<"best move: ";
        cout<<best_play[0]<<" "<<best_play[1]<<"\nbest score: "<<best_play[2]<<endl;
        cout<<"best move 2: "<<best_play2[0]<<" "<<best_play2[1]<<" score: "<<best_play2[2]<<endl;;
        avg_time+=duration_ms.count();
        avg_count++;
        pushPiece(getLowestRow(best_play[0],best_play[1],curPiece),best_play[0],best_play[1],curPiece);
        debugScore=1;
        cout<<"score: "<<getScoreOfGrid()<<endl;
        debugScore=0;
        
        
        



        //2
        actuallyPutThePiece(best_play[0],best_play[1]);



        //3 (done)
        capture();
        load_grid();
        curPiece=curQueue[0];
        curQueue=getQueue();        
        //break;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    cout<<"average time per move: ";
    cout<<fixed<<setprecision(4)<<avg_time/avg_count<<endl;
    
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);

    return 0;
}
