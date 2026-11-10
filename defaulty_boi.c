/*
 * defaulty_boi.c -- defaulty boi hitting the default dance in your terminal.
 * Build: cc -O2 -std=c11 defaulty_boi.c -o default_dance -lm
 * Run:   ./default_dance                 (space: pause, +/-: speed, q: quit)
 *        ./default_dance --frame 17      (print a plain ASCII still)
 *
 * real character geometry + recorded motion are loaded from an asset.
 * joint interpolation, skinning, projection, depth, and lighting run live in C.
 * bake the asset using the prepared Unity project. See unity/README.md.
 * Model/dance: Jonesy #2 package / Epic Games. See assets/README.md.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 100, HEIGHT = 50, FPS = 30 };
static const char ramp[] = " .,:;+*#%@";
static struct termios saved_termios;
static int raw_input, screen_active;
static volatile sig_atomic_t stopping, suspending;

typedef struct { float x,y,z; } Vec;
typedef struct { float x,y,z,w; } Quat;
typedef struct { Vec position,normal; float albedo; uint16_t joints[4]; float weights[4]; } Vertex;
typedef struct { Quat q; Vec translation; } Key;
typedef struct { float r[9]; Vec t; } Transform;
typedef struct { float x,y,z,nx,ny,nz,albedo; } Projected;
typedef struct {
    uint32_t vertex_count,index_count,bone_count,frame_count;
    float fps,duration;
    Vertex *vertices;
    uint32_t *indices;
    Key *keys;
    Projected *projected;
} Model;
static Model model;
static double view_angle=-15.0;
static const double pi=3.14159265358979323846;

static Vec vec(float x,float y,float z) { return (Vec){x,y,z}; }
static Vec add(Vec a,Vec b) { return vec(a.x+b.x,a.y+b.y,a.z+b.z); }
static Vec mul(Vec a,float s) { return vec(a.x*s,a.y*s,a.z*s); }
static float dot(Vec a,Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static Vec unit(Vec a) { return mul(a,1.0f/sqrtf(fmaxf(dot(a,a),1e-12f))); }
static float clamp(float x,float a,float b) { return fmaxf(a,fminf(x,b)); }

static void free_model(void)
{
    free(model.vertices); free(model.indices); free(model.keys); free(model.projected);
    memset(&model,0,sizeof model);
}

static int load_model(const char *path)
{
    FILE *f=fopen(path,"rb");
    if (!f) { fprintf(stderr,"Cannot open %s: %s\nExport with Unity's Dance menu, or use --asset PATH.\n",path,strerror(errno)); return 0; }
    char magic[8];
    uint32_t h[5];
    float timing[2];
    int ok=fread(magic,1,8,f)==8 && !memcmp(magic,"DANCE3D\0",8) &&
           fread(h,sizeof *h,5,f)==5 && fread(timing,sizeof *timing,2,f)==2;
    if (!ok || h[0]!=1 || !h[1] || h[1]>1000000 || !h[2] || h[2]>6000000 ||
        h[2]%3 || !h[3] || h[3]>256 || h[4]<2 || h[4]>10000 ||
        !isfinite(timing[0]) || timing[0]<1 || timing[0]>240 ||
        !isfinite(timing[1]) || timing[1]<=0 || timing[1]>3600) goto invalid;
    model.vertex_count=h[1]; model.index_count=h[2]; model.bone_count=h[3]; model.frame_count=h[4];
    model.fps=timing[0]; model.duration=timing[1];
    if (fabsf((model.frame_count-1)/model.fps-model.duration)>.001f) goto invalid;
    model.vertices=calloc(model.vertex_count,sizeof *model.vertices);
    model.indices=calloc(model.index_count,sizeof *model.indices);
    model.keys=calloc((size_t)model.bone_count*model.frame_count,sizeof *model.keys);
    model.projected=calloc(model.vertex_count,sizeof *model.projected);
    if (!model.vertices || !model.indices || !model.keys || !model.projected) goto invalid;
    _Static_assert(sizeof(Vertex)==52,"Vertex layout must match the asset format");
    _Static_assert(sizeof(Key)==28,"Key layout must match the asset format");
    if (fread(model.vertices,sizeof *model.vertices,model.vertex_count,f)!=model.vertex_count ||
        fread(model.indices,sizeof *model.indices,model.index_count,f)!=model.index_count ||
        fread(model.keys,sizeof *model.keys,(size_t)model.bone_count*model.frame_count,f)!=(size_t)model.bone_count*model.frame_count ||
        fgetc(f)!=EOF) goto invalid;
    for (uint32_t i=0;i<model.vertex_count;++i) {
        Vertex *v=&model.vertices[i];
        if (!isfinite(v->position.x) || !isfinite(v->position.y) || !isfinite(v->position.z) ||
            fabsf(v->position.x)>100 || fabsf(v->position.y)>100 || fabsf(v->position.z)>100 ||
            !isfinite(v->normal.x) || !isfinite(v->normal.y) || !isfinite(v->normal.z) ||
            !isfinite(v->albedo) || v->albedo<0 || v->albedo>1) goto invalid;
        float total=0;
        for (int k=0;k<4;++k) {
            if (v->joints[k]>=model.bone_count || !isfinite(v->weights[k]) ||
                v->weights[k]<0 || v->weights[k]>1) goto invalid;
            total+=v->weights[k];
        }
        if (fabsf(total-1)>.01f) goto invalid;
    }
    for (uint32_t i=0;i<model.index_count;++i)
        if (model.indices[i]>=model.vertex_count) goto invalid;
    for (size_t i=0;i<(size_t)model.bone_count*model.frame_count;++i) {
        Key *k=&model.keys[i];
        float length=k->q.x*k->q.x+k->q.y*k->q.y+k->q.z*k->q.z+k->q.w*k->q.w;
        if (!isfinite(length) || fabsf(length-1)>.01f ||
            !isfinite(k->translation.x) || !isfinite(k->translation.y) || !isfinite(k->translation.z) ||
            fabsf(k->translation.x)>100 || fabsf(k->translation.y)>100 || fabsf(k->translation.z)>100) goto invalid;
    }
    fclose(f);
    return 1;
invalid:
    fclose(f); free_model();
    fprintf(stderr,"Invalid, unsupported, or incomplete model asset: %s\n",path);
    return 0;
}

static Quat slerp(Quat a,Quat b,float t)
{
    float d=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
    if (d<0) { b=(Quat){-b.x,-b.y,-b.z,-b.w}; d=-d; }
    float x=1-t,y=t;
    if (d<.9995f) {
        float angle=acosf(clamp(d,-1,1)), divisor=sinf(angle);
        x=sinf((1-t)*angle)/divisor; y=sinf(t*angle)/divisor;
    }
    Quat q={a.x*x+b.x*y,a.y*x+b.y*y,a.z*x+b.z*y,a.w*x+b.w*y};
    float inverse=1/sqrtf(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    return (Quat){q.x*inverse,q.y*inverse,q.z*inverse,q.w*inverse};
}

static Transform transform(Quat q,Vec t)
{
    float x=q.x,y=q.y,z=q.z,w=q.w;
    return (Transform){{1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),
                        2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),
                        2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)},t};
}

static Vec rotate(Transform t,Vec v)
{
    return vec(t.r[0]*v.x+t.r[1]*v.y+t.r[2]*v.z,
               t.r[3]*v.x+t.r[4]*v.y+t.r[5]*v.z,
               t.r[6]*v.x+t.r[7]*v.y+t.r[8]*v.z);
}

static float edge(float ax,float ay,float bx,float by,float px,float py)
{ return (bx-ax)*(py-ay)-(by-ay)*(px-ax); }

static void render(double seconds,int w,int h,unsigned char *pixels)
{
    enum { MAX_W=320,MAX_H=160 };
    static float depth[MAX_W*MAX_H],light[MAX_W*MAX_H];
    Transform bones[256];
    float frame=clamp((float)seconds*model.fps,0,(float)model.frame_count-1);
    uint32_t a=(uint32_t)frame,b=a+1<model.frame_count ? a+1 : a;
    float blend=frame-a;
    for (uint32_t j=0;j<model.bone_count;++j) {
        Key x=model.keys[(size_t)a*model.bone_count+j],y=model.keys[(size_t)b*model.bone_count+j];
        bones[j]=transform(slerp(x.q,y.q,blend),add(mul(x.translation,1-blend),mul(y.translation,blend)));
    }
    int sw=w*2,sh=h*2;
    memset(depth,0,(size_t)sw*sh*sizeof *depth);
    memset(light,0,(size_t)sw*sh*sizeof *light);
    float yaw=(float)(view_angle*pi/180),c=cosf(yaw),s=sinf(yaw);
    float scale=sh/2.15f;
    for (uint32_t i=0;i<model.vertex_count;++i) {
        Vertex v=model.vertices[i];
        Vec p=vec(0,0,0),n=vec(0,0,0);
        for (int k=0;k<4;++k) if (v.weights[k]>0) {
            Transform t=bones[v.joints[k]];
            p=add(p,mul(add(rotate(t,v.position),t.t),v.weights[k]));
            n=add(n,mul(rotate(t,v.normal),v.weights[k]));
        }
        p=vec(c*p.x+s*p.z,p.y,-s*p.x+c*p.z);
        n=unit(vec(c*n.x+s*n.z,n.y,-s*n.x+c*n.z));
        float inverse=1/(4-p.z), perspective=4*inverse;
        model.projected[i]=(Projected){sw*.5f+p.x*scale*2*perspective,
                                      sh*.5f-(p.y-.90f)*scale*perspective,
                                      inverse,n.x,n.y,n.z,v.albedo};
    }
    const Vec lamp={-.4082483f,.4082483f,.8164966f};
    const Vec halfway={-.2141865f,.2141865f,.9530206f};
    for (uint32_t i=0;i<model.index_count;i+=3) {
        Projected p=model.projected[model.indices[i]],q=model.projected[model.indices[i+1]],r=model.projected[model.indices[i+2]];
        float area=edge(p.x,p.y,q.x,q.y,r.x,r.y);
        if (fabsf(area)<1e-5f) continue;
        int x0=(int)fmaxf(0,floorf(fminf(p.x,fminf(q.x,r.x))));
        int x1=(int)fminf(sw-1,ceilf(fmaxf(p.x,fmaxf(q.x,r.x))));
        int y0=(int)fmaxf(0,floorf(fminf(p.y,fminf(q.y,r.y))));
        int y1=(int)fminf(sh-1,ceilf(fmaxf(p.y,fmaxf(q.y,r.y))));
        for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) {
            float u=edge(q.x,q.y,r.x,r.y,x+.5f,y+.5f)/area;
            float v=edge(r.x,r.y,p.x,p.y,x+.5f,y+.5f)/area;
            float z=1-u-v;
            if (u<0 || v<0 || z<0) continue;
            float inv=u*p.z+v*q.z+z*r.z;
            size_t at=(size_t)y*sw+x;
            if (inv<=depth[at]) continue;
            depth[at]=inv;
            u=u*p.z/inv; v=v*q.z/inv; z=z*r.z/inv;
            Vec normal=unit(vec(u*p.nx+v*q.nx+z*r.nx,u*p.ny+v*q.ny+z*r.ny,u*p.nz+v*q.nz+z*r.nz));
            if (normal.z<0 && area>0) normal=mul(normal,-1);
            float diffuse=fmaxf(0,dot(normal,lamp));
            float specular=powf(fmaxf(0,dot(normal,halfway)),20)*.10f;
            float albedo=u*p.albedo+v*q.albedo+z*r.albedo;
            light[at]=clamp(powf(albedo,.55f)*(.30f+.70f*diffuse)+specular,.12f,1);
        }
    }
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        size_t at=(size_t)y*2*sw+x*2;
        float value=(light[at]+light[at+1]+light[at+sw]+light[at+sw+1])*.25f;
        if (value>0) value=fmaxf(value,.06f);
        pixels[y*w+x]=(unsigned char)clamp(floorf(value*9+.5f),0,9);
    }
}

static void restore_terminal(void)
{
    if (raw_input) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        raw_input = 0;
    }
    if (screen_active) {
        fputs("\033[0m\033[?25h\033[?1049l", stdout);
        fflush(stdout);
        screen_active = 0;
    }
}

static void on_signal(int sig)
{
    if (sig == SIGTSTP) suspending = 1;
    else stopping = 1;
}

static void enter_terminal(void)
{
    struct termios mode = saved_termios;
    if (isatty(STDIN_FILENO)) {
        mode.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        mode.c_cc[VMIN] = 0;
        mode.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &mode) == 0) raw_input = 1;
    }
    screen_active = 1;
    fputs("\033[?1049h\033[?25l\033[0m\033[2J", stdout);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void terminal_size(int *cols, int *rows)
{
    struct winsize size;
    *cols = 80;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        if (size.ws_col) *cols = size.ws_col;
        if (size.ws_row) *rows = size.ws_row;
    }
}

static int draw(double seconds, int paused, double speed, int invert)
{
    static int old_cols, old_rows;
    unsigned char pixels[160*80];
    char out[20000], status[120];
    int cols, rows;
    size_t pos=0;
    terminal_size(&cols,&rows);
    if (cols<20 || rows<10)
        return fputs("\033[H\033[2JResize terminal\r\n(q to quit)",stdout)>=0 && fflush(stdout)==0;
    int w=cols-2<160 ? cols-2 : 160;
    int h=rows-3<80 ? rows-3 : 80;
    int left=(cols-w)/2, top=(rows-h-2)/2;
    render(seconds,w,h,pixels);
    pos+=(size_t)snprintf(out+pos,sizeof out-pos,
                         cols==old_cols && rows==old_rows ? "\033[H" : "\033[H\033[2J");
    old_cols=cols; old_rows=rows;
    for (int y=0;y<h;++y) {
        pos+=(size_t)snprintf(out+pos,sizeof out-pos,"\033[%d;%dH",top+y+1,left+1);
        for (int x=0;x<w;++x) {
            int value=pixels[y*w+x];
            out[pos++]=ramp[invert ? 9-value : value];
        }
    }
    snprintf(status,sizeof status,"%s%.2fx  [space] %s  [+/-] speed  [a/d] rotate  [q] quit",
             paused ? "PAUSED  " : "",speed,paused ? "play" : "pause");
    int length=(int)strlen(status);
    if (length>cols-2) length=cols-2;
    pos+=(size_t)snprintf(out+pos,sizeof out-pos,"\033[%d;1H\033[K\033[%d;%dH%.*s",
                         top+h+2,top+h+2,(cols-length)/2+1,length,status);
    return fwrite(out,1,pos,stdout)==pos && fflush(stdout)==0;
}

static void usage(FILE *out, const char *name)
{
    fprintf(out,
        "Usage: %s [--once] [--speed 0.25..4] [--angle degrees] [--invert]\n"
        "       %s --frame index [--invert]\n\n"
        "Jonesy mesh + recorded dance, skinned and shaded live in ASCII.\n"
        "Controls: space pause/play, +/- speed (0.25x steps), r restart, a/d rotate, q/Esc quit.\n"
        "Use a 100-column, 53-row terminal or larger for more detail.\n"
        "--asset PATH selects a model asset (default assets/jonesy-unity.dance).\n"
        "--frame prints a still without terminal control sequences.\n", name, name);
}

int main(int argc, char **argv)
{
    int once = 0, still = -1, paused = 0, invert = 0, success = 1;
    double speed = 1.0, elapsed = 0.0;
    const char *asset_path = "assets/jonesy-unity.dance";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout, argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--invert")) invert = 1;
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            char *end;
            const char *arg = argv[++i];
            speed = strtod(arg, &end);
            if (end == arg || *end || !isfinite(speed) || speed < 0.25 || speed > 4.0)
                goto bad_argument;
        } else if (!strcmp(argv[i], "--asset") && i + 1 < argc) {
            asset_path = argv[++i];
        } else if (!strcmp(argv[i], "--angle") && i + 1 < argc) {
            char *end;
            const char *arg = argv[++i];
            double angle = strtod(arg, &end);
            if (end == arg || *end || !isfinite(angle)) goto bad_argument;
            view_angle = fmod(angle, 360);
        } else if (!strcmp(argv[i], "--frame") && i + 1 < argc) {
            char *end;
            const char *arg = argv[++i];
            long n = strtol(arg, &end, 10);
            if (end == arg || *end || n < 0 || n >= 10000) goto bad_argument;
            still = (int)n;
        } else goto bad_argument;
    }
    if (!load_model(asset_path)) return 1;
    atexit(free_model);
    if (still >= 0) {
        if ((uint32_t)still >= model.frame_count) {
            fprintf(stderr,"Frame index must be between 0 and %u for this asset.\n",model.frame_count-1);
            return 2;
        }
        unsigned char pixels[WIDTH * HEIGHT];
        render(still / (double)model.fps, WIDTH, HEIGHT, pixels);
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                int v = pixels[y * WIDTH + x];
                if (putchar(ramp[invert ? 9 - v : v]) == EOF) return 1;
            }
            if (putchar('\n') == EOF) return 1;
        }
        return fflush(stdout) == 0 ? 0 : 1;
    }
    if (!isatty(STDOUT_FILENO)) {
        fputs("Animation needs a terminal. Use --frame 17 to print a still.\n", stderr);
        return 1;
    }
    const char *term = getenv("TERM");
    if (term && !strcmp(term, "dumb")) {
        fputs("Use an ANSI terminal, or --frame 17 for a plain ASCII still.\n", stderr);
        return 1;
    }
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_termios) < 0) {
        perror("tcgetattr");
        return 1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGTSTP, &action, NULL);
    atexit(restore_terminal);
    enter_terminal();
    double previous = now();
    const double duration = model.duration;
    int interactive = isatty(STDIN_FILENO);
    while (!stopping) {
        double current = now();
        if (!paused) elapsed += (current - previous) * speed;
        previous = current;
        if (once && elapsed >= duration) break;
        if (!draw(fmod(elapsed, duration), paused, speed, invert)) {
            success = 0;
            break;
        }
        if (suspending) {
            restore_terminal();
            suspending = 0;
            raise(SIGSTOP);
            if (stopping) break;
            enter_terminal();
            previous = now();
        }
        fd_set readers;
        FD_ZERO(&readers);
        if (interactive) FD_SET(STDIN_FILENO, &readers);
        struct timeval timeout = {0, 1000000 / FPS};
        int ready = select(interactive ? STDIN_FILENO + 1 : 0,
                           &readers, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            success = 0;
            break;
        }
        if (ready > 0) {
            char keys[32];
            ssize_t n = read(STDIN_FILENO, keys, sizeof keys);
            current = now();
            if (!paused) elapsed += (current - previous) * speed;
            previous = current;
            for (ssize_t k = 0; k < n; ++k) {
                switch (keys[k]) {
                case 'q': case 'Q': case 27: stopping = 1; break;
                case ' ': paused = !paused; break;
                case '+': case '=': speed = fmin(4.0, speed + 0.25); break;
                case '-': case '_': speed = fmax(0.25, speed - 0.25); break;
                case 'r': case 'R': elapsed = 0; break;
                case 'a': view_angle = fmod(view_angle - 10, 360); break;
                case 'd': view_angle = fmod(view_angle + 10, 360); break;
                default: break;
                }
            }
        }
    }
    restore_terminal();
    return success ? 0 : 1;

bad_argument:
    usage(stderr, argv[0]);
    return 2;
}
