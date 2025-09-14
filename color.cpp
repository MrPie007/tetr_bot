#include <windows.h>
#include <bits/stdc++.h>
#include <chrono>
#include <thread>
using namespace std;
HDC hScreenDC;
HDC hMemoryDC;
BITMAPINFO bmi;
HBITMAP hBitmap;
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
    color c;
    //cells[0] is the original rotation, up or x will make it cells[1]
    vector<vector<pair<int,int>>>cells;
    char type;
    Piece(color c,vector<vector<pair<int,int>>>cells,char type):c(c),cells(cells),type(type){}
};
//handling rotations will suck so much
//I can just assume they are new pieces basically
//how do I get current piece??
Piece IPiece({124,254,198},{{{-1,0},{0,0},{1,0},{2,0}}},'I');
Piece JPiece({148,144,222},{{{0,0},{1,0},{-1,0},{-1,-1}}},'J');
Piece LPiece({250,165,126},{{{0,0},{1,0},{-1,0},{1,-1}}},'L');
Piece OPiece({255,227,130},{{{0,0},{1,0},{0,-1},{1,-1}}},'O');
Piece ZPiece({252,137,143},{{{0,0},{1,0},{0,-1},{-1,-1}}},'Z');
Piece SPiece({196,250,136},{{{0,0},{-1,0},{0,-1},{1,-1}}},'S');
Piece TPiece({231,135,211},{{{0,0},{-1,0},{1,0},{0,-1}}},'T');
Piece all_p[7]={IPiece,JPiece,LPiece,OPiece,ZPiece,SPiece,TPiece};
char all_pc[7]={'I','J','L','O','Z','S','T'};
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
int grid[30][10];
int cell_size=35;
int grid_width=350,grid_height=700;
//I will make 0th row the top row
//row 20 is the floor, it is always filled with 1s
//for clarityp
//Pieces are defined by center piece, and positions of other pieces relative to it

void load_grid()
{
    for(int i=15;i<grid_width;i+=cell_size)
    {
        for(int j=15;j<grid_height;j+=cell_size)
        {
            color cur = getMaxInRegion(i,j,i+2,j+2);
            if(cur.R>70 || cur.G>70 || cur.B>70)
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
void popPiece(int i,int j, Piece p)
{
    for(int k=0;k<p.cells[0].size();k++)
    {
        int ni = i+p.cells[0][k].second;
        int nj = j+p.cells[0][k].first;
        grid[ni][nj]=0;
    }
}
void pushPiece(int i,int j, Piece p)
{
    for(int k=0;k<p.cells[0].size();k++)
    {
        int ni = i+p.cells[0][k].second;
        int nj = j+p.cells[0][k].first;
        grid[ni][nj]=1;
    }
}
int getLowestRow(int j, Piece p)
{
    //j is the position for the "core" of the piece
    //I need to put it as down as possible
    int to_put=-1;
    ///HERE I also reduce space from top of grid..
    //this should be really optimizable
    for(int i=3;i<=20;i++)
    {
        bool valid=1;
        for(int k=0;k<p.cells[0].size();k++)
        {
            int ni = i+p.cells[0][k].second;
            int nj = j+p.cells[0][k].first;
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
void actuallyPutThePiece(int pos,int rotateCount)
{
    //4 is the default position of all pieces
    //TODO, implement the ability to rotate
    //TODO, add the coordinates of rotated pieces for each piece
    int curPos=4;
    while(curPos>pos)
    {
        pressKey(VK_LEFT);
        curPos--;
        Sleep(50);
    }
    while(curPos<pos)
    {
        pressKey(VK_RIGHT);
        curPos++;
        Sleep(50);
    }
    pressKey(VK_SPACE);
    Sleep(50);


}
int getGridHeight()
{
    for(int i=0;i<=20;i++)
    {
        for(int j=0;j<10;j++)
        {
            if(grid[i][j])return 20-i;
        }
    }
    return -1;
}
//Small heuristic of minimizing the max height
int getBestPos(Piece p)
{
    int mnHeight=25;
    int mnHeightInd=4;
    for(int j=0;j<10;j++)
    {
        int x=getLowestRow(j,p);
        if(x!=-1)
        {
            pushPiece(x,j,p);
            int curHeight = getGridHeight();
            if(curHeight<mnHeight)
            {
                mnHeight=curHeight;
                mnHeightInd=j;
            }
            popPiece(x,j,p);
        }
    }
    return mnHeightInd;
}
int main() {
    init();
    //I will need to use the extra space at the top later, will implement as if I don't need it tho rn
    srand(2);
    for(int j=0;j<10;j++)
    {
        grid[20][j]=1;
    }
    if (!pPixels) {
        std::cerr << "Failed to create DIB section" << std::endl;
        return -1;
    }
    
    double milliseconds = 0;
    auto start = std::chrono::high_resolution_clock::now();
    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    int c=0;
    //This is for starting the game
    while (true) {
        if (GetAsyncKeyState(0x50) & 0x8000) {
            cout<<"Starting game..";
            break;
        }
        Sleep(50); // small delay so CPU isn't 100% busy
    }
    capture(); 
    save_pic();
    load_grid();
    vector<Piece>curQueue = getQueue();
    //I need to put a piece first before bot taking over
    while (true) {
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
            Sleep(400);
            break;
        }
        Sleep(50);
    }
    capture();    
    load_grid();
    save_pic();
    Piece curPiece = curQueue[0];
    curQueue = getQueue();
    /*
        Flow of logic should be something like

        1. find best place for current state
        2. put piece in that place
        3. get new statep
    */
    while (1) 
    {   
        //if I press P stop the bot (fail safe instead of ctrl c from terminal)
        if (GetAsyncKeyState(0x50) & 0x8000) {
            break;
        }

        //1
        int best_pos = getBestPos(curPiece);



        //2
        actuallyPutThePiece(best_pos,0);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));


        //3 (done)
        capture();
        load_grid();
        curPiece=curQueue[0];
        curQueue=getQueue();        
    }
    
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);

    return 0;
}
