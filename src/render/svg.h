#pragma once
//
// svg.h — SVG renderer for Helix.
//
// Rasterizes SVG elements into a bitmap. Supports rect (with rx/ry), circle,
// ellipse, line, polyline, polygon, path (M/L/H/V/C/S/Q/T/A/Z with subpaths),
// use/defs, transform attribute, viewBox scaling, opacity, style attribute,
// rgb()/rgba()/hsl() colors, all 148 CSS named colors, fill-rule evenodd,
// stroke on all shapes.
//
#include "html/dom.h"
#include "html/parser.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <functional>

struct SvgBitmap {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels; // RGBA
};

namespace svg {

// ── Color ───────────────────────────────────────────────────────────────────

struct Color { uint8_t r=0,g=0,b=0,a=255; };

inline Color parseColor(const std::string& s) {
    if (s.empty() || s == "none" || s == "transparent") return {0,0,0,0};
    if (s[0] == '#') {
        try {
            if (s.size()==4) { unsigned long v=std::stoul(s.substr(1),nullptr,16); return {(uint8_t)(((v>>8)&0xF)*17),(uint8_t)(((v>>4)&0xF)*17),(uint8_t)((v&0xF)*17),255}; }
            if (s.size()>=7) { unsigned long v=std::stoul(s.substr(1,6),nullptr,16); return {(uint8_t)(v>>16),(uint8_t)((v>>8)&0xFF),(uint8_t)(v&0xFF),255}; }
        } catch (...) {}
    }
    if (s.rfind("rgb",0)==0) {
        size_t p=s.find('('),e=s.rfind(')');
        if (p!=std::string::npos&&e!=std::string::npos) {
            std::string inner=s.substr(p+1,e-p-1); float v[4]={0,0,0,1}; int n=0;
            std::istringstream ss(inner); std::string tok;
            while (std::getline(ss,tok,',')&&n<4) {
                while(!tok.empty()&&tok[0]==' ')tok.erase(tok.begin());
                try{v[n++]=std::stof(tok);}catch(...){}
            }
            return {(uint8_t)std::clamp(v[0],0.f,255.f),(uint8_t)std::clamp(v[1],0.f,255.f),
                    (uint8_t)std::clamp(v[2],0.f,255.f),(uint8_t)(std::clamp(v[3],0.f,1.f)*255)};
        }
    }
    if (s.rfind("hsl",0)==0) {
        size_t p=s.find('('),e=s.rfind(')');
        if (p!=std::string::npos&&e!=std::string::npos) {
            float h=0,sat=0,l=0; std::string inner=s.substr(p+1,e-p-1);
            if (sscanf(inner.c_str(),"%f,%f%%,%f%%",&h,&sat,&l)>=3||sscanf(inner.c_str(),"%f %f%% %f%%",&h,&sat,&l)>=3) {
                sat/=100;l/=100;
                auto hue2rgb=[](float p,float q,float t){if(t<0)t+=1;if(t>1)t-=1;if(t<1.f/6)return p+(q-p)*6*t;if(t<.5f)return q;if(t<2.f/3)return p+(q-p)*(2.f/3-t)*6;return p;};
                float q=l<.5f?l*(1+sat):l+sat-l*sat,pp=2*l-q,hf=h/360.f;
                return {(uint8_t)(hue2rgb(pp,q,hf+1.f/3)*255),(uint8_t)(hue2rgb(pp,q,hf)*255),(uint8_t)(hue2rgb(pp,q,hf-1.f/3)*255),255};
            }
        }
    }
    static const std::map<std::string,uint32_t> named={
        {"aliceblue",0xF0F8FF},{"antiquewhite",0xFAEBD7},{"aqua",0x00FFFF},{"aquamarine",0x7FFFD4},{"azure",0xF0FFFF},{"beige",0xF5F5DC},{"bisque",0xFFE4C4},{"black",0x000000},{"blanchedalmond",0xFFEBCD},{"blue",0x0000FF},{"blueviolet",0x8A2BE2},{"brown",0xA52A2A},{"burlywood",0xDEB887},{"cadetblue",0x5F9EA0},{"chartreuse",0x7FFF00},{"chocolate",0xD2691E},{"coral",0xFF7F50},{"cornflowerblue",0x6495ED},{"cornsilk",0xFFF8DC},{"crimson",0xDC143C},{"cyan",0x00FFFF},{"darkblue",0x00008B},{"darkcyan",0x008B8B},{"darkgoldenrod",0xB8860B},{"darkgray",0xA9A9A9},{"darkgreen",0x006400},{"darkgrey",0xA9A9A9},{"darkkhaki",0xBDB76B},{"darkmagenta",0x8B008B},{"darkolivegreen",0x556B2F},{"darkorange",0xFF8C00},{"darkorchid",0x9932CC},{"darkred",0x8B0000},{"darksalmon",0xE9967A},{"darkseagreen",0x8FBC8F},{"darkslateblue",0x483D8B},{"darkslategray",0x2F4F4F},{"darkslategrey",0x2F4F4F},{"darkturquoise",0x00CED1},{"darkviolet",0x9400D3},{"deeppink",0xFF1493},{"deepskyblue",0x00BFFF},{"dimgray",0x696969},{"dimgrey",0x696969},{"dodgerblue",0x1E90FF},{"firebrick",0xB22222},{"floralwhite",0xFFFAF0},{"forestgreen",0x228B22},{"fuchsia",0xFF00FF},{"gainsboro",0xDCDCDC},{"ghostwhite",0xF8F8FF},{"gold",0xFFD700},{"goldenrod",0xDAA520},{"gray",0x808080},{"green",0x008000},{"greenyellow",0xADFF2F},{"grey",0x808080},{"honeydew",0xF0FFF0},{"hotpink",0xFF69B4},{"indianred",0xCD5C5C},{"indigo",0x4B0082},{"ivory",0xFFFFF0},{"khaki",0xF0E68C},{"lavender",0xE6E6FA},{"lavenderblush",0xFFF0F5},{"lawngreen",0x7CFC00},{"lemonchiffon",0xFFFACD},{"lightblue",0xADD8E6},{"lightcoral",0xF08080},{"lightcyan",0xE0FFFF},{"lightgoldenrodyellow",0xFAFAD2},{"lightgray",0xD3D3D3},{"lightgreen",0x90EE90},{"lightgrey",0xD3D3D3},{"lightpink",0xFFB6C1},{"lightsalmon",0xFFA07A},{"lightseagreen",0x20B2AA},{"lightskyblue",0x87CEFA},{"lightslategray",0x778899},{"lightslategrey",0x778899},{"lightsteelblue",0xB0C4DE},{"lightyellow",0xFFFFE0},{"lime",0x00FF00},{"limegreen",0x32CD32},{"linen",0xFAF0E6},{"magenta",0xFF00FF},{"maroon",0x800000},{"mediumaquamarine",0x66CDAA},{"mediumblue",0x0000CD},{"mediumorchid",0xBA55D3},{"mediumpurple",0x9370DB},{"mediumseagreen",0x3CB371},{"mediumslateblue",0x7B68EE},{"mediumspringgreen",0x00FA9A},{"mediumturquoise",0x48D1CC},{"mediumvioletred",0xC71585},{"midnightblue",0x191970},{"mintcream",0xF5FFFA},{"mistyrose",0xFFE4E1},{"moccasin",0xFFE4B5},{"navajowhite",0xFFDEAD},{"navy",0x000080},{"oldlace",0xFDF5E6},{"olive",0x808000},{"olivedrab",0x6B8E23},{"orange",0xFFA500},{"orangered",0xFF4500},{"orchid",0xDA70D6},{"palegoldenrod",0xEEE8AA},{"palegreen",0x98FB98},{"paleturquoise",0xAFEEEE},{"palevioletred",0xDB7093},{"papayawhip",0xFFEFD5},{"peachpuff",0xFFDAB9},{"peru",0xCD853F},{"pink",0xFFC0CB},{"plum",0xDDA0DD},{"powderblue",0xB0E0E6},{"purple",0x800080},{"rebeccapurple",0x663399},{"red",0xFF0000},{"rosybrown",0xBC8F8F},{"royalblue",0x4169E1},{"saddlebrown",0x8B4513},{"salmon",0xFA8072},{"sandybrown",0xF4A460},{"seagreen",0x2E8B57},{"seashell",0xFFF5EE},{"sienna",0xA0522D},{"silver",0xC0C0C0},{"skyblue",0x87CEEB},{"slateblue",0x6A5ACD},{"slategray",0x708090},{"slategrey",0x708090},{"snow",0xFFFAFA},{"springgreen",0x00FF7F},{"steelblue",0x4682B4},{"tan",0xD2B48C},{"teal",0x008080},{"thistle",0xD8BFD8},{"tomato",0xFF6347},{"turquoise",0x40E0D0},{"violet",0xEE82EE},{"wheat",0xF5DEB3},{"white",0xFFFFFF},{"whitesmoke",0xF5F5F5},{"yellow",0xFFFF00},{"yellowgreen",0x9ACD32}
    };
    std::string low=s; for(auto&c:low) c=(char)std::tolower((unsigned char)c);
    auto it=named.find(low); if(it!=named.end()){uint32_t v=it->second;return{(uint8_t)(v>>16),(uint8_t)((v>>8)&0xFF),(uint8_t)(v&0xFF),255};}
    return {0,0,0,255};
}

// ── Transform matrix ────────────────────────────────────────────────────────

struct Mat { float a=1,b=0,c=0,d=1,e=0,f=0; };
inline Mat matMul(const Mat&l,const Mat&r){return{l.a*r.a+l.c*r.b,l.b*r.a+l.d*r.b,l.a*r.c+l.c*r.d,l.b*r.c+l.d*r.d,l.a*r.e+l.c*r.f+l.e,l.b*r.e+l.d*r.f+l.f};}
inline void txPt(const Mat&m,float&x,float&y){float nx=m.a*x+m.c*y+m.e,ny=m.b*x+m.d*y+m.f;x=nx;y=ny;}

inline Mat parseTransform(const std::string& s) {
    Mat result; size_t pos=0;
    while(pos<s.size()){
        while(pos<s.size()&&!std::isalpha((unsigned char)s[pos]))++pos;
        size_t ns=pos; while(pos<s.size()&&std::isalpha((unsigned char)s[pos]))++pos;
        std::string fn=s.substr(ns,pos-ns);
        size_t p=s.find('(',pos),e=s.find(')',p!=std::string::npos?p:pos);
        if(p==std::string::npos||e==std::string::npos)break;
        std::string args=s.substr(p+1,e-p-1); float v[6]={}; int n=0;
        std::istringstream ss(args); float fv;
        // Split on comma or space
        std::string tok; while(n<6&&std::getline(ss,tok,',')){std::istringstream s2(tok);while(n<6&&s2>>fv)v[n++]=fv;}
        Mat m;
        if(fn=="translate"){m.e=v[0];m.f=n>=2?v[1]:0;}
        else if(fn=="scale"){m.a=v[0];m.d=n>=2?v[1]:v[0];}
        else if(fn=="rotate"){float rad=v[0]*3.14159265f/180.f,cs=std::cos(rad),sn=std::sin(rad);
            if(n>=3){m={cs,sn,-sn,cs,v[1]*(1-cs)+v[2]*sn,v[2]*(1-cs)-v[1]*sn};}
            else{m={cs,sn,-sn,cs,0,0};}}
        else if(fn=="skewX"){m.c=std::tan(v[0]*3.14159265f/180.f);}
        else if(fn=="skewY"){m.b=std::tan(v[0]*3.14159265f/180.f);}
        else if(fn=="matrix"&&n>=6){m={v[0],v[1],v[2],v[3],v[4],v[5]};}
        result=matMul(result,m); pos=e+1;
    }
    return result;
}

// ── Drawing ─────────────────────────────────────────────────────────────────

inline float parseNum(const std::string& s){try{return std::stof(s);}catch(...){return 0;}}

inline void blendPixel(SvgBitmap& bmp, int x, int y, Color c) {
    if(x<0||x>=bmp.width||y<0||y>=bmp.height||c.a==0)return;
    int idx=(y*bmp.width+x)*4;
    if(c.a==255){bmp.pixels[idx]=c.r;bmp.pixels[idx+1]=c.g;bmp.pixels[idx+2]=c.b;bmp.pixels[idx+3]=255;}
    else{float a=c.a/255.f,ia=1.f-a;bmp.pixels[idx]=(uint8_t)(c.r*a+bmp.pixels[idx]*ia);bmp.pixels[idx+1]=(uint8_t)(c.g*a+bmp.pixels[idx+1]*ia);bmp.pixels[idx+2]=(uint8_t)(c.b*a+bmp.pixels[idx+2]*ia);bmp.pixels[idx+3]=std::max(bmp.pixels[idx+3],c.a);}
}

struct Pt{float x,y;};

inline void fillPoly(SvgBitmap& bmp,const std::vector<Pt>& pts,Color c,const Mat& m){
    if(pts.size()<3||c.a==0)return;
    std::vector<Pt> tp(pts.size()); float minY=1e9f,maxY=-1e9f;
    for(size_t i=0;i<pts.size();++i){tp[i]=pts[i];txPt(m,tp[i].x,tp[i].y);minY=std::min(minY,tp[i].y);maxY=std::max(maxY,tp[i].y);}
    for(int y=std::max(0,(int)minY);y<=std::min(bmp.height-1,(int)(maxY+1));++y){
        std::vector<float> xs;
        for(size_t j=0;j+1<tp.size();++j){float y0=tp[j].y,y1=tp[j+1].y;if((y0<=y&&y1>y)||(y1<=y&&y0>y))xs.push_back(tp[j].x+((float)y-y0)/(y1-y0)*(tp[j+1].x-tp[j].x));}
        std::sort(xs.begin(),xs.end());
        for(size_t j=0;j+1<xs.size();j+=2){int x0=std::max(0,(int)(xs[j]+.5f)),x1=std::min(bmp.width-1,(int)(xs[j+1]+.5f));for(int x=x0;x<=x1;++x)blendPixel(bmp,x,y,c);}
    }
}

inline void strokePoly(SvgBitmap& bmp,const std::vector<Pt>& pts,Color c,float sw,const Mat& m){
    if(pts.size()<2||c.a==0||sw<=0)return;
    float half=sw/2.f;
    for(size_t i=0;i+1<pts.size();++i){
        float x1=pts[i].x,y1=pts[i].y,x2=pts[i+1].x,y2=pts[i+1].y;
        txPt(m,x1,y1);txPt(m,x2,y2);
        float dx=x2-x1,dy=y2-y1,steps=std::max(std::abs(dx),std::abs(dy));
        if(steps<1)steps=1; float ix=dx/steps,iy=dy/steps;
        for(float s=0;s<=steps;s+=1.f){float px=x1+ix*s,py=y1+iy*s;
            for(int ty=(int)(py-half);ty<=(int)(py+half);++ty)for(int tx=(int)(px-half);tx<=(int)(px+half);++tx)blendPixel(bmp,tx,ty,c);}
    }
}

inline void circleAsPoly(float cx,float cy,float r,std::vector<Pt>& pts){
    int segs=std::clamp((int)(r*2),16,64);
    for(int i=0;i<=segs;++i){float a=2.f*3.14159265f*i/segs;pts.push_back({cx+r*std::cos(a),cy+r*std::sin(a)});}
}

inline void ellipseAsPoly(float cx,float cy,float rx,float ry,std::vector<Pt>& pts){
    int segs=std::clamp((int)(std::max(rx,ry)*2),16,64);
    for(int i=0;i<=segs;++i){float a=2.f*3.14159265f*i/segs;pts.push_back({cx+rx*std::cos(a),cy+ry*std::sin(a)});}
}

// ── Path parser ─────────────────────────────────────────────────────────────

inline void parsePath(const std::string& d, std::vector<std::vector<Pt>>& subpaths) {
    std::vector<Pt> poly; float cx=0,cy=0,startX=0,startY=0,lx2=0,ly2=0; char lastCmd=0;
    size_t i=0;
    auto skipWS=[&](){while(i<d.size()&&(d[i]==' '||d[i]==','||d[i]=='\n'||d[i]=='\r'||d[i]=='\t'))++i;};
    auto readNum=[&]()->float{
        skipWS();if(i>=d.size())return 0;size_t start=i;
        if(i<d.size()&&(d[i]=='-'||d[i]=='+'))++i;
        bool dot=false;while(i<d.size()&&(std::isdigit((unsigned char)d[i])||(d[i]=='.'&&!dot))){if(d[i]=='.')dot=true;++i;}
        if(i<d.size()&&(d[i]=='e'||d[i]=='E')){++i;if(i<d.size()&&(d[i]=='-'||d[i]=='+'))++i;while(i<d.size()&&std::isdigit((unsigned char)d[i]))++i;}
        if(i==start)return 0;try{return std::stof(d.substr(start,i-start));}catch(...){return 0;}
    };
    auto hasNum=[&]()->bool{size_t j=i;while(j<d.size()&&(d[j]==' '||d[j]==','||d[j]=='\n'))++j;return j<d.size()&&(std::isdigit((unsigned char)d[j])||d[j]=='-'||d[j]=='+'||d[j]=='.');};
    auto cubic=[&](float x0,float y0,float x1,float y1,float x2,float y2,float x3,float y3){
        for(int s=1;s<=20;++s){float t=(float)s/20,u=1-t;poly.push_back({u*u*u*x0+3*u*u*t*x1+3*u*t*t*x2+t*t*t*x3,u*u*u*y0+3*u*u*t*y1+3*u*t*t*y2+t*t*t*y3});}};
    auto quad=[&](float x0,float y0,float x1,float y1,float x2,float y2){
        for(int s=1;s<=16;++s){float t=(float)s/16,u=1-t;poly.push_back({u*u*x0+2*u*t*x1+t*t*x2,u*u*y0+2*u*t*y1+t*t*y2});}};
    auto arcTo=[&](float rx,float ry,float xrot,bool la,bool sw,float x,float y){
        if(rx<=0||ry<=0){poly.push_back({x,y});return;}
        float dx2=(cx-x)/2,dy2=(cy-y)/2,cosA=std::cos(xrot*3.14159265f/180),sinA=std::sin(xrot*3.14159265f/180);
        float x1p=cosA*dx2+sinA*dy2,y1p=-sinA*dx2+cosA*dy2;
        float r2=rx*rx*ry*ry-rx*rx*y1p*y1p-ry*ry*x1p*x1p,dn=rx*rx*y1p*y1p+ry*ry*x1p*x1p;
        if(dn<=0){poly.push_back({x,y});return;}
        float sq=std::sqrt(std::max(0.f,r2/dn));if(la==sw)sq=-sq;
        float cxp=sq*rx*y1p/ry,cyp=-sq*ry*x1p/rx;
        float mx=(cx+x)/2,my=(cy+y)/2,ccx=cosA*cxp-sinA*cyp+mx,ccy=sinA*cxp+cosA*cyp+my;
        auto angle=[](float ux,float uy,float vx,float vy){return std::atan2(ux*vy-uy*vx,ux*vx+uy*vy);};
        float t1=angle(1,0,(x1p-cxp)/rx,(y1p-cyp)/ry),dt=angle((x1p-cxp)/rx,(y1p-cyp)/ry,(-x1p-cxp)/rx,(-y1p-cyp)/ry);
        if(!sw&&dt>0)dt-=2*3.14159265f;if(sw&&dt<0)dt+=2*3.14159265f;
        int segs=std::max(4,(int)(std::abs(dt)/0.3f));
        for(int s=1;s<=segs;++s){float t=t1+dt*s/segs;poly.push_back({cosA*rx*std::cos(t)-sinA*ry*std::sin(t)+ccx,sinA*rx*std::cos(t)+cosA*ry*std::sin(t)+ccy});}
    };

    while(i<d.size()){
        skipWS();if(i>=d.size())break;
        char cmd=d[i]; if(std::isalpha((unsigned char)cmd)){++i;}else cmd=lastCmd;
        bool rel=(cmd>='a'&&cmd<='z'); char C=rel?(cmd-32):cmd;
        do{
            switch(C){
            case 'M':if(!poly.empty()){subpaths.push_back(poly);poly.clear();}
                {float x=readNum(),y=readNum();if(rel){x+=cx;y+=cy;}cx=startX=x;cy=startY=y;poly.push_back({cx,cy});C='L';lastCmd=rel?'l':'L';break;}
            case 'L':{float x=readNum(),y=readNum();if(rel){x+=cx;y+=cy;}cx=x;cy=y;poly.push_back({cx,cy});break;}
            case 'H':{float x=readNum();if(rel)x+=cx;cx=x;poly.push_back({cx,cy});break;}
            case 'V':{float y=readNum();if(rel)y+=cy;cy=y;poly.push_back({cx,cy});break;}
            case 'C':{float x1=readNum(),y1=readNum(),x2=readNum(),y2=readNum(),x=readNum(),y=readNum();
                if(rel){x1+=cx;y1+=cy;x2+=cx;y2+=cy;x+=cx;y+=cy;}cubic(cx,cy,x1,y1,x2,y2,x,y);lx2=x2;ly2=y2;cx=x;cy=y;break;}
            case 'S':{float rx1=2*cx-lx2,ry1=2*cy-ly2,x2=readNum(),y2=readNum(),x=readNum(),y=readNum();
                if(rel){x2+=cx;y2+=cy;x+=cx;y+=cy;}cubic(cx,cy,rx1,ry1,x2,y2,x,y);lx2=x2;ly2=y2;cx=x;cy=y;break;}
            case 'Q':{float x1=readNum(),y1=readNum(),x=readNum(),y=readNum();
                if(rel){x1+=cx;y1+=cy;x+=cx;y+=cy;}quad(cx,cy,x1,y1,x,y);lx2=x1;ly2=y1;cx=x;cy=y;break;}
            case 'T':{float rx1=2*cx-lx2,ry1=2*cy-ly2,x=readNum(),y=readNum();
                if(rel){x+=cx;y+=cy;}quad(cx,cy,rx1,ry1,x,y);lx2=rx1;ly2=ry1;cx=x;cy=y;break;}
            case 'A':{float rx=readNum(),ry=readNum(),xrot=readNum(),la=readNum(),sw=readNum(),x=readNum(),y=readNum();
                if(rel){x+=cx;y+=cy;}arcTo(rx,ry,xrot,la!=0,sw!=0,x,y);cx=x;cy=y;break;}
            case 'Z':poly.push_back({startX,startY});subpaths.push_back(poly);poly.clear();cx=startX;cy=startY;break;
            default:++i;goto next;
            }
            if(C!='M')lastCmd=cmd;
        }while(C!='Z'&&hasNum());
        next:;
    }
    if(!poly.empty())subpaths.push_back(poly);
}

// ── Style ───────────────────────────────────────────────────────────────────

struct Style{Color fill={0,0,0,255};Color stroke={0,0,0,0};float sw=1;float opacity=1;float fillOp=1;float strokeOp=1;bool fillNone=false;bool strokeNone=false;};

inline Style getStyle(const Node* node,const Style& parent){
    Style s=parent;
    std::map<std::string,std::string> props;
    std::string sa=node->attr("style");
    if(!sa.empty()){std::istringstream ss(sa);std::string decl;while(std::getline(ss,decl,';')){size_t c=decl.find(':');if(c==std::string::npos)continue;std::string k=decl.substr(0,c),v=decl.substr(c+1);while(!k.empty()&&k[0]==' ')k.erase(k.begin());while(!k.empty()&&k.back()==' ')k.pop_back();while(!v.empty()&&v[0]==' ')v.erase(v.begin());while(!v.empty()&&v.back()==' ')v.pop_back();props[k]=v;}}
    auto get=[&](const std::string&name)->std::string{std::string v=node->attr(name);if(v.empty()){auto it=props.find(name);if(it!=props.end())v=it->second;}return v;};
    std::string f=get("fill");if(!f.empty()){if(f=="none")s.fillNone=true;else{s.fill=parseColor(f);s.fillNone=false;}}
    std::string st=get("stroke");if(!st.empty()){if(st=="none")s.strokeNone=true;else{s.stroke=parseColor(st);s.strokeNone=false;}}
    std::string sw=get("stroke-width");if(!sw.empty())try{s.sw=std::stof(sw);}catch(...){}
    std::string op=get("opacity");if(!op.empty())try{s.opacity=std::stof(op);}catch(...){}
    std::string fo=get("fill-opacity");if(!fo.empty())try{s.fillOp=std::stof(fo);}catch(...){}
    std::string so=get("stroke-opacity");if(!so.empty())try{s.strokeOp=std::stof(so);}catch(...){}
    return s;
}

inline Color applyOp(Color c,float op,float chOp){c.a=(uint8_t)(std::clamp((c.a/255.f)*op*chOp,0.f,1.f)*255);return c;}

// ── Render ──────────────────────────────────────────────────────────────────

inline void collectDefs(const Node* n,std::map<std::string,const Node*>& defs){
    if(!n)return;std::string id=n->attr("id");if(!id.empty())defs[id]=n;
    for(auto&c:n->children)collectDefs(c.get(),defs);
}

inline void renderEl(SvgBitmap& bmp,const Node* node,const Mat& parentM,const Style& parentStyle,const std::map<std::string,const Node*>& defs){
    if(!node||node->type!=NodeType::Element)return;
    const std::string&tag=node->tagName;
    if(tag=="defs"||tag=="clippath"||tag=="lineargradient"||tag=="radialgradient"||tag=="symbol"||tag=="mask"||tag=="title"||tag=="desc"||tag=="metadata")return;

    Style style=getStyle(node,parentStyle);
    Color fill=style.fillNone?Color{0,0,0,0}:applyOp(style.fill,style.opacity,style.fillOp);
    Color stroke=style.strokeNone?Color{0,0,0,0}:applyOp(style.stroke,style.opacity,style.strokeOp);
    float sw=style.sw;
    Mat m=parentM;std::string tr=node->attr("transform");if(!tr.empty())m=matMul(m,parseTransform(tr));

    if(tag=="rect"){
        float x=parseNum(node->attr("x")),y=parseNum(node->attr("y")),w=parseNum(node->attr("width")),h=parseNum(node->attr("height"));
        float rx=parseNum(node->attr("rx")),ry=parseNum(node->attr("ry"));if(ry<=0)ry=rx;if(rx<=0)rx=ry;
        if(rx>0){
            rx=std::min(rx,w/2);ry=std::min(ry,h/2);
            std::vector<Pt>pts;
            auto corner=[&](float ccx,float ccy,float sa,int n){for(int j=0;j<=n;++j){float a=sa+3.14159265f/2*j/n;pts.push_back({ccx+rx*std::cos(a),ccy+ry*std::sin(a)});}};
            corner(x+w-rx,y+ry,-3.14159265f/2,8);corner(x+w-rx,y+h-ry,0,8);corner(x+rx,y+h-ry,3.14159265f/2,8);corner(x+rx,y+ry,3.14159265f,8);
            pts.push_back(pts[0]);
            if(fill.a>0)fillPoly(bmp,pts,fill,m);if(stroke.a>0)strokePoly(bmp,pts,stroke,sw,m);
        }else{
            std::vector<Pt>pts={{x,y},{x+w,y},{x+w,y+h},{x,y+h},{x,y}};
            if(fill.a>0)fillPoly(bmp,pts,fill,m);if(stroke.a>0)strokePoly(bmp,pts,stroke,sw,m);
        }
    }else if(tag=="circle"){
        float ccx=parseNum(node->attr("cx")),ccy=parseNum(node->attr("cy")),r=parseNum(node->attr("r"));
        std::vector<Pt>pts;circleAsPoly(ccx,ccy,r,pts);
        if(fill.a>0)fillPoly(bmp,pts,fill,m);if(stroke.a>0)strokePoly(bmp,pts,stroke,sw,m);
    }else if(tag=="ellipse"){
        float ccx=parseNum(node->attr("cx")),ccy=parseNum(node->attr("cy")),rx=parseNum(node->attr("rx")),ry=parseNum(node->attr("ry"));
        std::vector<Pt>pts;ellipseAsPoly(ccx,ccy,rx,ry,pts);
        if(fill.a>0)fillPoly(bmp,pts,fill,m);if(stroke.a>0)strokePoly(bmp,pts,stroke,sw,m);
    }else if(tag=="line"){
        float x1=parseNum(node->attr("x1")),y1=parseNum(node->attr("y1")),x2=parseNum(node->attr("x2")),y2=parseNum(node->attr("y2"));
        strokePoly(bmp,{{x1,y1},{x2,y2}},stroke.a>0?stroke:fill,sw,m);
    }else if(tag=="path"){
        std::string d=node->attr("d");if(!d.empty()){
            std::vector<std::vector<Pt>>sps;parsePath(d,sps);
            for(auto&sp:sps){if(fill.a>0&&sp.size()>=3)fillPoly(bmp,sp,fill,m);if(stroke.a>0&&sp.size()>=2)strokePoly(bmp,sp,stroke,sw,m);}
        }
    }else if(tag=="polygon"||tag=="polyline"){
        std::string pts=node->attr("points");std::vector<Pt>poly;std::istringstream ss(pts);float x,y;
        while(ss>>x){char c;if(ss.peek()==',')ss>>c;ss>>y;poly.push_back({x,y});}
        if(tag=="polygon"&&!poly.empty())poly.push_back(poly[0]);
        if(fill.a>0&&poly.size()>=3)fillPoly(bmp,poly,fill,m);if(stroke.a>0&&poly.size()>=2)strokePoly(bmp,poly,stroke,sw,m);
    }else if(tag=="use"){
        std::string href=node->attr("href");if(href.empty())href=node->attr("xlink:href");
        if(!href.empty()&&href[0]=='#'){auto it=defs.find(href.substr(1));if(it!=defs.end()){
            float ux=parseNum(node->attr("x")),uy=parseNum(node->attr("y"));
            Mat um=matMul(m,Mat{1,0,0,1,ux,uy});renderEl(bmp,it->second,um,style,defs);}}
    }
    for(auto&child:node->children)renderEl(bmp,child.get(),m,style,defs);
}

// ── Entry point ─────────────────────────────────────────────────────────────

inline SvgBitmap renderSvg(const Node* svgNode, int maxDim=512){
    SvgBitmap bmp;if(!svgNode)return bmp;
    float vx=0,vy=0,vw=0,vh=0;
    std::string vb=svgNode->attr("viewBox");if(vb.empty())vb=svgNode->attr("viewbox");
    if(!vb.empty()){std::istringstream ss(vb);char c;if(!(ss>>vx>>c>>vy>>c>>vw>>c>>vh)||vw<=0){ss.clear();ss.str(vb);ss>>vx>>vy>>vw>>vh;}}
    float w=parseNum(svgNode->attr("width")),h=parseNum(svgNode->attr("height"));
    if(w<=0&&vw>0)w=vw;if(h<=0&&vh>0)h=vh;if(w<=0||h<=0)return bmp;
    float scale=1.f;if(w>maxDim||h>maxDim)scale=maxDim/std::max(w,h);
    bmp.width=std::max(1,(int)(w*scale));bmp.height=std::max(1,(int)(h*scale));
    bmp.pixels.resize(bmp.width*bmp.height*4,0);
    Mat viewM;
    if(vw>0&&vh>0){float sx=w/vw,sy=h/vh,s=std::min(sx,sy);float tx=(w-vw*s)/2-vx*s,ty=(h-vh*s)/2-vy*s;viewM={s*scale,0,0,s*scale,tx*scale,ty*scale};}
    else viewM={scale,0,0,scale,0,0};
    std::map<std::string,const Node*>defs;collectDefs(svgNode,defs);
    Style rootStyle;std::string rf=svgNode->attr("fill");if(!rf.empty()&&rf!="none")rootStyle.fill=parseColor(rf);
    for(auto&child:svgNode->children)renderEl(bmp,child.get(),viewM,rootStyle,defs);
    return bmp;
}

// ── Utility ─────────────────────────────────────────────────────────────────

inline bool looksLikeSvgBytes(const std::vector<uint8_t>& bytes){
    if(bytes.empty())return false;const size_t n=std::min<size_t>(bytes.size(),512);
    std::string head;head.reserve(n);for(size_t i=0;i<n;++i)head+=(char)std::tolower((unsigned char)bytes[i]);
    return head.find("<svg")!=std::string::npos||head.find("<?xml")!=std::string::npos||head.find("image/svg+xml")!=std::string::npos;
}

inline const Node* findSvgNode(const Node* node){
    if(!node)return nullptr;if(node->type==NodeType::Element&&node->tagName=="svg")return node;
    for(const auto&child:node->children)if(const Node*found=findSvgNode(child.get()))return found;return nullptr;
}

inline SvgBitmap renderSvgBytes(const std::string& text,int maxDim=512){auto doc=ParseHtml(text);const Node*svgNode=findSvgNode(doc.get());return svgNode?renderSvg(svgNode,maxDim):SvgBitmap{};}
inline SvgBitmap renderSvgBytes(const std::vector<uint8_t>& bytes,int maxDim=512){return renderSvgBytes(std::string(bytes.begin(),bytes.end()),maxDim);}

} // namespace svg
