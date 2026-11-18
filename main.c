/*
 * ============================================================
 *  Interactive 3D Rubik's Cube Simulator
 *  Computer Graphics & Visualization Lab - Mini Project
 *  Language : C (C99)
 *  Library  : OpenGL (legacy fixed-function) + GLUT / freeglut
 * ============================================================
 *
 *  CONTROLS
 *    Left mouse drag     : rotate the whole view
 *    U D R L F B         : turn that face clockwise
 *    Shift + U D R L F B : turn that face counter-clockwise
 *    S : scramble (20 random moves)     Z : undo last move
 *    V : solve (replay history backwards)  X : reset    Esc : quit
 *    H : show or hide the key legend
 *    Right click         : menu with the same actions
 *
 *  SELF-TEST (no window): run the program with  --test
 *
 *  Face directions are fixed to the cube, not the screen:
 *  U = up (white), D = down (yellow), R = right (red),
 *  L = left (orange), F = front (green), B = back (blue).
 * ============================================================
 */

#ifdef _WIN32
#include <windows.h>          /* needed before GL headers on some Windows setups */
#endif
#include <GL/glut.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D   /* missing from old Windows gl.h */
#endif
#define PI 3.14159265f

#define ANIM_FRAMES   18      /* frames used for one 90-degree turn          */
#define FRAME_MS      16      /* delay between frames (about 60 fps)         */
#define MAX_MOVES     1024    /* size of the history stack and move queue    */
#define SCRAMBLE_LEN  20
#define SPACING       1.0f    /* distance between neighbouring cubie centres */
#define BODY_HALF     0.48f   /* half-size of a cubie (0.48 < 0.5 gives gaps)*/
#define STICKER_HALF  0.41f   /* half-size of a coloured sticker             */
#define STICKER_ROUND 0.09f   /* corner radius of a sticker                  */

/* ------------------------------------------------------------
 *  DATA STRUCTURES
 * ------------------------------------------------------------ */

/* One of the 27 small cubes. */
typedef struct {
    int pos[3];      /* where it is NOW: each coordinate is -1, 0 or +1      */
    int home[3];     /* where it was when solved; decides its sticker colours */
    int rot[3][3];   /* orientation: rot * (local direction) = world direction */
} Cubie;

/* One face turn. */
typedef struct {
    int face;        /* 0..5 = U D R L F B                                    */
    int dir;         /* +1 = clockwise, -1 = counter-clockwise, as seen when
                        looking straight at that face from outside the cube   */
} Move;

/* A move waiting to be animated. */
typedef struct {
    Move m;
    int  record;     /* 1 = push onto history when done (normal moves),
                        0 = don't (moves produced by undo / solve)            */
} QueuedMove;

/* Face table: each face turns the layer where coordinate AXIS equals LAYER. */
static const char FACE_NAME[6]  = { 'U', 'D', 'R', 'L', 'F', 'B' };
static const int  FACE_AXIS[6]  = {  1,   1,   0,   0,   2,   2  }; /* 0=x 1=y 2=z */
static const int  FACE_LAYER[6] = { +1,  -1,  +1,  -1,  +1,  -1  };

/* Colours, Western scheme: white up, green front, red right. */
enum { WHITE, YELLOW, RED, ORANGE, GREEN, BLUE, NONE };
static const GLfloat COLOR[6][3] = {
    {0.96f, 0.97f, 0.98f},   /* white  #F4F6F8 */
    {1.00f, 0.84f, 0.00f},   /* yellow #FFD500 */
    {0.77f, 0.12f, 0.23f},   /* red    #C41E3A */
    {1.00f, 0.35f, 0.00f},   /* orange #FF5800 */
    {0.00f, 0.62f, 0.38f},   /* green  #009E60 */
    {0.00f, 0.32f, 0.73f}    /* blue   #0051BA */
};

/* ------------------------------------------------------------
 *  GLOBAL STATE
 * ------------------------------------------------------------ */
static Cubie cube[27];

static Move       history[MAX_MOVES];   /* every recorded move, oldest first */
static int        historyCount = 0;
static QueuedMove queue[MAX_MOVES];     /* moves waiting to be animated      */
static int        qHead = 0, qTail = 0;

static int   animating  = 0;    /* 1 while a face is turning             */
static Move  animMove;          /* the move currently being animated     */
static int   animRecord = 0;
static int   animFrame  = 0;
static float animAngle  = 0.0f; /* current angle of the turning layer    */
static int   animId     = 0;    /* generation number, see timerFunc      */

static float viewRotX = 25.0f, viewRotY = -35.0f;  /* camera angles */
static int   dragging = 0, lastX, lastY;
static int   winW = 960, winH = 760;
static int   showKeys = 1;          /* H toggles the key legend */

/* ------------------------------------------------------------
 *  CUBE LOGIC (no OpenGL here, so it can be self-tested)
 * ------------------------------------------------------------ */

/* Index of the cubie whose home is (x,y,z); matches resetCube's order. */
static int homeIndex(int x, int y, int z) { return (x + 1) * 9 + (y + 1) * 3 + (z + 1); }

/* Put every cubie back at home with no rotation (identity matrix). */
static void resetCube(void)
{
    int x, y, z, i, j;
    for (x = -1; x <= 1; x++)
        for (y = -1; y <= 1; y++)
            for (z = -1; z <= 1; z++) {
                Cubie *c = &cube[homeIndex(x, y, z)];
                c->pos[0] = c->home[0] = x;
                c->pos[1] = c->home[1] = y;
                c->pos[2] = c->home[2] = z;
                for (i = 0; i < 3; i++)
                    for (j = 0; j < 3; j++)
                        c->rot[i][j] = (i == j);
            }
}

/*
 * How many quarter turns about the POSITIVE axis a move is (+1 or -1).
 * Clockwise seen from outside a face is a negative rotation about that
 * face's outward normal (right-hand rule). For U, R, F the normal is the
 * +axis; for D, L, B it is the -axis, which flips the sign.
 */
static int quarterTurns(Move m) { return -FACE_LAYER[m.face] * m.dir; }

/* Exact integer matrix for a 90-degree turn (s = +1 or -1) about an axis.
   cos 90 = 0 and sin 90 = 1, so every entry is 0, 1 or -1. */
static void makeRotation(int axis, int s, int M[3][3])
{
    memset(M, 0, sizeof(int) * 9);
    if (axis == 0)      { M[0][0] = 1; M[1][2] = -s; M[2][1] =  s; }  /* about x */
    else if (axis == 1) { M[1][1] = 1; M[0][2] =  s; M[2][0] = -s; }  /* about y */
    else                { M[2][2] = 1; M[0][1] = -s; M[1][0] =  s; }  /* about z */
}

/*
 * THE STATE UPDATE. Called once, when a turn's animation finishes.
 * 1. Pick the cubies in the turning layer (pos[axis] == layer).
 * 2. Rotate each one's position:     newPos = M * pos
 * 3. Rotate each one's orientation:  newRot = M * rot
 * Everything is integer arithmetic, so there is no floating-point drift.
 */
static void applyMove(Move m)
{
    int axis = FACE_AXIS[m.face], layer = FACE_LAYER[m.face];
    int M[3][3], i, r, c, k;
    makeRotation(axis, quarterTurns(m), M);

    for (i = 0; i < 27; i++) {
        int p[3], R[3][3];
        if (cube[i].pos[axis] != layer) continue;      /* not in this layer */

        for (r = 0; r < 3; r++) {
            p[r] = 0;
            for (k = 0; k < 3; k++) p[r] += M[r][k] * cube[i].pos[k];
        }
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++) {
                R[r][c] = 0;
                for (k = 0; k < 3; k++) R[r][c] += M[r][k] * cube[i].rot[k][c];
            }
        memcpy(cube[i].pos, p, sizeof p);
        memcpy(cube[i].rot, R, sizeof R);
    }
}

/* Strict check: every cubie is at home with identity orientation. */
static int isSolved(void)
{
    int i, r, c;
    for (i = 0; i < 27; i++) {
        for (r = 0; r < 3; r++) {
            if (cube[i].pos[r] != cube[i].home[r]) return 0;
            for (c = 0; c < 3; c++)
                if (cube[i].rot[r][c] != (r == c)) return 0;
        }
    }
    return 1;
}

static Move inverse(Move m) { m.dir = -m.dir; return m; }

/* ------------------------------------------------------------
 *  MOVE QUEUE, ANIMATION AND ACTIONS
 * ------------------------------------------------------------ */
static void timerFunc(int id);

static int busy(void) { return animating || qHead != qTail; }

static void enqueue(Move m, int record)
{
    if (qTail >= MAX_MOVES) return;          /* queue full: ignore */
    queue[qTail].m = m;
    queue[qTail].record = record;
    qTail++;
}

/* If nothing is turning, take the next queued move and start animating it. */
static void startNextMove(void)
{
    QueuedMove q;
    if (animating || qHead == qTail) return;
    q = queue[qHead++];
    if (qHead == qTail) qHead = qTail = 0;   /* queue empty: rewind */

    animMove   = q.m;
    animRecord = q.record;
    animFrame  = 0;
    animAngle  = 0.0f;
    animating  = 1;
    animId++;
    glutTimerFunc(FRAME_MS, timerFunc, animId);
}

/*
 * Animation tick. The angle grows from 0 to +/-90 degrees in ANIM_FRAMES
 * steps. Only on the LAST frame is the logical state updated (applyMove),
 * so the picture never jumps. The id check ignores stale timers left over
 * from an animation that Reset cancelled; otherwise two timers could run
 * at once and turns would play at double speed.
 */
static void timerFunc(int id)
{
    if (!animating || id != animId) return;

    animFrame++;
    {   /* smoothstep easing: starts slow, speeds up, settles gently */
        float t = (float)animFrame / ANIM_FRAMES;
        float e = t * t * (3.0f - 2.0f * t);
        animAngle = 90.0f * quarterTurns(animMove) * e;
    }

    if (animFrame >= ANIM_FRAMES) {
        applyMove(animMove);
        if (animRecord && historyCount < MAX_MOVES)
            history[historyCount++] = animMove;
        animating = 0;
        animAngle = 0.0f;
        startNextMove();                     /* continue a scramble / solve */
    } else {
        glutTimerFunc(FRAME_MS, timerFunc, id);
    }
    glutPostRedisplay();
}

static void doTurn(int face, int dir)
{
    Move m;
    if (busy()) return;                      /* block input while turning */
    m.face = face; m.dir = dir;
    enqueue(m, 1);
    startNextMove();
}

static void doScramble(void)
{
    int i, last = -1;
    if (busy()) return;
    for (i = 0; i < SCRAMBLE_LEN; i++) {
        Move m;
        do { m.face = rand() % 6; } while (m.face == last); /* no instant undo */
        m.dir = (rand() % 2) ? 1 : -1;
        last = m.face;
        enqueue(m, 1);
    }
    startNextMove();
}

static void doUndo(void)
{
    if (busy() || historyCount == 0) return;
    enqueue(inverse(history[--historyCount]), 0);
    startNextMove();
}

/* Solve = play the inverse of every recorded move, newest first. */
static void doSolve(void)
{
    if (busy()) return;
    while (historyCount > 0)
        enqueue(inverse(history[--historyCount]), 0);
    startNextMove();
}

/* Reset works even mid-animation: it cancels everything. */
static void doReset(void)
{
    animating = 0;
    animAngle = 0.0f;
    animId++;                                /* invalidate pending timers */
    qHead = qTail = 0;
    historyCount = 0;
    resetCube();
    glutPostRedisplay();
}

/* ------------------------------------------------------------
 *  DRAWING
 *  Look: a product shot. Light studio backdrop, the cube floating
 *  over a soft contact shadow, quiet slate UI so the six cube
 *  colours are the only bold thing on screen.
 * ------------------------------------------------------------ */

/* UI palette */
static const GLfloat BG_TOP[3]    = {0.937f, 0.945f, 0.957f};  /* #EFF1F4 */
static const GLfloat BG_BOTTOM[3] = {0.784f, 0.816f, 0.851f};  /* #C8D0D9 */
static const GLfloat INK[3]       = {0.122f, 0.153f, 0.188f};  /* #1F2730 */
static const GLfloat MUTED[3]     = {0.357f, 0.400f, 0.447f};  /* #5B6672 */
static const GLfloat KEY_EDGE[3]  = {0.682f, 0.718f, 0.761f};  /* #AEB7C2 */

#define FONT_TITLE GLUT_BITMAP_HELVETICA_18
#define FONT_BODY  GLUT_BITMAP_HELVETICA_12

/* ---------- 3D: the cube ---------- */

/* One flat square face of the black body: perpendicular to axis a,
   on side sg (+1/-1), at distance d from the cubie centre, half-size h. */
static void quad(int a, int sg, float d, float h)
{
    static const float corner[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
    int u = (a + 1) % 3, v = (a + 2) % 3, i;
    float n[3] = {0, 0, 0}, p[3];
    n[a] = (float)sg;
    glNormal3fv(n);                          /* normal for lighting */
    for (i = 0; i < 4; i++) {
        int k = (sg > 0) ? i : 3 - i;        /* keep counter-clockwise winding */
        p[a] = sg * d;
        p[u] = corner[k][0] * h;
        p[v] = corner[k][1] * h;
        glVertex3fv(p);
    }
}

/* A sticker: a square with rounded corners, made of 4 quarter-circle
   arcs joined into one convex polygon. Same plane rules as quad(). */
static void roundedSticker(int a, int sg, float d, float h, float r)
{
    const int SEG = 5;                       /* points per corner arc */
    int u = (a + 1) % 3, v = (a + 2) % 3, c, i, n = 0;
    float pts[4 * 6][2], nrm[3] = {0, 0, 0}, p[3];
    float cx[4] = { h - r, -(h - r), -(h - r),  h - r };
    float cy[4] = { h - r,   h - r , -(h - r), -(h - r) };

    for (c = 0; c < 4; c++)                  /* corners in counter-clockwise order */
        for (i = 0; i <= SEG; i++) {
            float ang = (c * 90.0f + i * 90.0f / SEG) * PI / 180.0f;
            pts[n][0] = cx[c] + r * cosf(ang);
            pts[n][1] = cy[c] + r * sinf(ang);
            n++;
        }

    nrm[a] = (float)sg;
    glBegin(GL_POLYGON);
    glNormal3fv(nrm);
    for (i = 0; i < n; i++) {
        int k = (sg > 0) ? i : n - 1 - i;    /* flip winding on negative sides */
        p[a] = sg * d;
        p[u] = pts[k][0];
        p[v] = pts[k][1];
        glVertex3fv(p);
    }
    glEnd();
}

/* Which colour a cubie's local face gets, decided by its HOME position:
   only faces that were on the outside of the solved cube get stickers. */
static int stickerColor(const Cubie *c, int a, int sg)
{
    if (c->home[a] != sg) return NONE;
    switch (a) {
        case 0:  return sg > 0 ? RED   : ORANGE;
        case 1:  return sg > 0 ? WHITE : YELLOW;
        default: return sg > 0 ? GREEN : BLUE;
    }
}

/* Draw one cubie at the origin in its own local coordinates:
   a dark plastic body with a faint sheen, then glossy stickers
   sitting just outside it. */
static void drawCubie(const Cubie *c)
{
    static const GLfloat bodySpec[4]    = {0.25f, 0.25f, 0.25f, 1.0f};
    static const GLfloat stickerSpec[4] = {0.55f, 0.55f, 0.55f, 1.0f};
    int a, sg, col;

    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, bodySpec);
    glMaterialf (GL_FRONT_AND_BACK, GL_SHININESS, 18.0f);
    glColor3f(0.08f, 0.09f, 0.10f);
    glBegin(GL_QUADS);
    for (a = 0; a < 3; a++)
        for (sg = -1; sg <= 1; sg += 2)
            quad(a, sg, BODY_HALF, BODY_HALF);
    glEnd();

    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, stickerSpec);
    glMaterialf (GL_FRONT_AND_BACK, GL_SHININESS, 60.0f);
    for (a = 0; a < 3; a++)
        for (sg = -1; sg <= 1; sg += 2) {
            col = stickerColor(c, a, sg);
            if (col == NONE) continue;
            glColor3fv(COLOR[col]);
            roundedSticker(a, sg, BODY_HALF + 0.004f, STICKER_HALF, STICKER_ROUND);
        }
}

/* Soft shadow on an imaginary floor under the cube: a disc that is
   darkest in the middle and fades to transparent at the edge. */
static void drawContactShadow(float strength)
{
    const int SEG = 48;
    const float y = -2.35f, rIn = 1.3f, rOut = 2.7f;
    int i;
    if (strength <= 0.0f) return;

    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);                   /* shadow must not hide the cube */

    glBegin(GL_TRIANGLE_FAN);                /* dark centre */
    glColor4f(0.10f, 0.13f, 0.17f, 0.30f * strength);
    glVertex3f(0, y, 0);
    glColor4f(0.10f, 0.13f, 0.17f, 0.16f * strength);
    for (i = 0; i <= SEG; i++) {
        float t = i * 2.0f * PI / SEG;
        glVertex3f(rIn * cosf(t), y, rIn * sinf(t));
    }
    glEnd();

    glBegin(GL_QUAD_STRIP);                  /* fade-out ring */
    for (i = 0; i <= SEG; i++) {
        float t = i * 2.0f * PI / SEG;
        glColor4f(0.10f, 0.13f, 0.17f, 0.16f * strength);
        glVertex3f(rIn * cosf(t), y, rIn * sinf(t));
        glColor4f(0.10f, 0.13f, 0.17f, 0.0f);
        glVertex3f(rOut * cosf(t), y, rOut * sinf(t));
    }
    glEnd();

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

/* ---------- 2D: backdrop and interface ---------- */

/* Switch to flat pixel coordinates (origin bottom-left) for 2D drawing. */
static void begin2D(void)
{
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    gluOrtho2D(0, winW, 0, winH);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
}

static void end2D(void)
{
    glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
}

/* Vertical gradient filling the whole window. */
static void drawBackdrop(void)
{
    begin2D();
    glBegin(GL_QUADS);
    glColor3fv(BG_BOTTOM); glVertex2i(0, 0);    glVertex2i(winW, 0);
    glColor3fv(BG_TOP);    glVertex2i(winW, winH); glVertex2i(0, winH);
    glEnd();
    end2D();
}

/* Outline of a rounded rectangle, as vertices (use inside glBegin). */
static void roundRectPath(float x, float y, float w, float h, float r)
{
    const int SEG = 6;
    float cx[4] = { x + w - r, x + r, x + r, x + w - r };
    float cy[4] = { y + h - r, y + h - r, y + r, y + r };
    int c, i;
    for (c = 0; c < 4; c++)
        for (i = 0; i <= SEG; i++) {
            float ang = (c * 90.0f + i * 90.0f / SEG) * PI / 180.0f;
            glVertex2f(cx[c] + r * cosf(ang), cy[c] + r * sinf(ang));
        }
}

static void fillRoundRect(float x, float y, float w, float h, float r)
{
    glBegin(GL_POLYGON); roundRectPath(x, y, w, h, r); glEnd();
}

static void strokeRoundRect(float x, float y, float w, float h, float r)
{
    glEnable(GL_LINE_SMOOTH);
    glBegin(GL_LINE_LOOP); roundRectPath(x, y, w, h, r); glEnd();
    glDisable(GL_LINE_SMOOTH);
}

static int textWidth(void *font, const char *s)
{
    return glutBitmapLength(font, (const unsigned char *)s);
}

static void drawText(float x, float y, void *font, const char *s)
{
    glRasterPos2f(x, y);
    while (*s) glutBitmapCharacter(font, *s++);
}

/* A keyboard key drawn as a small keycap. Returns its width. */
#define KEY_H 22
static int keycapWidth(const char *label)
{
    int w = textWidth(FONT_BODY, label) + 14;
    return w < KEY_H ? KEY_H : w;
}

static int drawKeycap(float x, float y, const char *label)
{
    int w = keycapWidth(label);
    glColor4f(KEY_EDGE[0], KEY_EDGE[1], KEY_EDGE[2], 0.9f);  /* key "depth" */
    fillRoundRect(x, y - 2, (float)w, KEY_H, 5);
    glColor3f(1, 1, 1);
    fillRoundRect(x, y, (float)w, KEY_H, 5);
    glColor3fv(KEY_EDGE);
    strokeRoundRect(x, y, (float)w, KEY_H, 5);
    glColor3fv(INK);
    drawText(x + (w - textWidth(FONT_BODY, label)) / 2.0f, y + 7, FONT_BODY, label);
    return w;
}

/* The key legend at the bottom: groups of keycaps followed by what they do. */
typedef struct { const char *keys[6]; int nkeys; const char *label; } LegendItem;
static const LegendItem LEGEND[] = {
    { {"U", "D", "R", "L", "F", "B"}, 6, "Turn a face" },
    { {"Shift"}, 1, "Turn the other way" },
    { {"S"}, 1, "Scramble" },
    { {"Z"}, 1, "Undo" },
    { {"V"}, 1, "Solve" },
    { {"X"}, 1, "Reset" },
    { {"H"}, 1, "Hide keys" }
};
#define LEGEND_N ((int)(sizeof LEGEND / sizeof LEGEND[0]))
#define KEY_GAP   4      /* between keycaps in a group  */
#define LABEL_GAP 8      /* between keycaps and label   */
#define ITEM_GAP  26     /* between groups              */

static int legendItemWidth(const LegendItem *it)
{
    int w = 0, k;
    for (k = 0; k < it->nkeys; k++) w += keycapWidth(it->keys[k]) + KEY_GAP;
    return w - KEY_GAP + LABEL_GAP + textWidth(FONT_BODY, it->label);
}

static void drawLegend(void)
{
    int rowOf[LEGEND_N], rowW[LEGEND_N], rows = 0, i, k;
    int maxW = winW - 48;
    float x = 0;

    /* Flow layout: fill a row until it is full, then wrap. */
    rowW[0] = 0;
    for (i = 0; i < LEGEND_N; i++) {
        int w = legendItemWidth(&LEGEND[i]);
        int add = (rowW[rows] == 0) ? w : w + ITEM_GAP;
        if (rowW[rows] > 0 && rowW[rows] + add > maxW) { rows++; rowW[rows] = 0; add = w; }
        rowW[rows] += add;
        rowOf[i] = rows;
    }
    rows++;

    for (i = 0; i < LEGEND_N; i++) {
        int r = rowOf[i];
        float y = 22.0f + (rows - 1 - r) * 34.0f;
        if (i == 0 || rowOf[i - 1] != r) x = (winW - rowW[r]) / 2.0f;  /* centre row */
        for (k = 0; k < LEGEND[i].nkeys; k++)
            x += drawKeycap(x, y, LEGEND[i].keys[k]) + KEY_GAP;
        x += LABEL_GAP - KEY_GAP;
        glColor3fv(MUTED);
        drawText(x, y + 7, FONT_BODY, LEGEND[i].label);
        x += textWidth(FONT_BODY, LEGEND[i].label) + ITEM_GAP;
    }
}

/* Status pill (top right): green only when solved. */
static void drawStatus(void)
{
    char moves[32];
    const char *label;
    int solved = !busy() && isSolved();
    float w, h = 26, x, y = winH - 24 - h;

    if (busy())       label = "Turning";
    else if (solved)  label = "Solved";
    else              label = "Scrambled";

    w = textWidth(FONT_BODY, label) + 28.0f;
    x = winW - 24 - w;

    if (solved)       glColor3fv(COLOR[GREEN]);
    else if (busy())  glColor3fv(MUTED);
    else              glColor3f(1, 1, 1);
    fillRoundRect(x, y, w, h, h / 2);
    if (!solved && !busy()) { glColor3fv(KEY_EDGE); strokeRoundRect(x, y, w, h, h / 2); }

    if (solved || busy()) glColor3f(1, 1, 1); else glColor3fv(INK);
    drawText(x + 14, y + 9, FONT_BODY, label);

    sprintf(moves, "Moves: %d", historyCount);
    glColor3fv(MUTED);
    drawText(winW - 24 - textWidth(FONT_BODY, moves), y - 20, FONT_BODY, moves);
}

static void drawHUD(void)
{
    begin2D();

    glColor3fv(INK);
    drawText(24, winH - 42, FONT_TITLE, "Rubik's Cube");
    glColor3fv(MUTED);
    drawText(24, winH - 62, FONT_BODY, "Drag to look around. Right-click for the menu.");

    drawStatus();

    if (showKeys) drawLegend();
    else {
        glColor3fv(MUTED);
        drawText(24, 22, FONT_BODY, "Press H to show the keys");
    }
    end2D();
}

static void display(void)
{
    /* Lights are positioned before the view rotation, so they stay fixed
       relative to the camera (w = 0 means a directional light). */
    static const GLfloat keyLight[4]  = {  3.0f, 4.0f, 5.0f, 0.0f };
    static const GLfloat fillLight[4] = { -5.0f, 0.5f, 2.0f, 0.0f };
    float tilt, shadow;
    int i, r, c;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawBackdrop();

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, keyLight);
    glLightfv(GL_LIGHT1, GL_POSITION, fillLight);

    glTranslatef(0.0f, 0.2f, -12.0f);        /* move the camera back */

    /* Vertical drag tilts the camera over the "floor"; horizontal drag
       spins only the cube. The shadow fades out when you look from below. */
    glRotatef(viewRotX, 1, 0, 0);
    tilt = sinf(viewRotX * PI / 180.0f);
    shadow = tilt * 1.6f;                    /* full shadow above ~40 degrees */
    if (shadow > 1.0f) shadow = 1.0f;
    drawContactShadow(shadow);   /* does nothing if shadow <= 0 */
    glRotatef(viewRotY, 0, 1, 0);

    for (i = 0; i < 27; i++) {
        GLfloat m[16] = {0};
        glPushMatrix();

        /* If this cubie is in the turning layer, rotate it by the
           animation angle about the layer's axis (through the centre). */
        if (animating && cube[i].pos[FACE_AXIS[animMove.face]] == FACE_LAYER[animMove.face]) {
            float ax[3] = {0, 0, 0};
            ax[FACE_AXIS[animMove.face]] = 1.0f;
            glRotatef(animAngle, ax[0], ax[1], ax[2]);
        }

        /* Move to the cubie's grid position, then apply its orientation.
           OpenGL matrices are column-major: m[col*4 + row]. */
        glTranslatef(cube[i].pos[0] * SPACING, cube[i].pos[1] * SPACING, cube[i].pos[2] * SPACING);
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++)
                m[c * 4 + r] = (GLfloat)cube[i].rot[r][c];
        m[15] = 1.0f;
        glMultMatrixf(m);

        drawCubie(&cube[i]);
        glPopMatrix();
    }

    drawHUD();
    glutSwapBuffers();
}

/* ------------------------------------------------------------
 *  CALLBACKS
 * ------------------------------------------------------------ */
static void reshape(int w, int h)
{
    if (h == 0) h = 1;
    winW = w; winH = h;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(35.0, (double)w / h, 1.0, 100.0);
    glMatrixMode(GL_MODELVIEW);
}

static void keyboard(unsigned char key, int x, int y)
{
    /* Use the Shift modifier, not letter case, so Caps Lock can't flip turns. */
    int shift = glutGetModifiers() & GLUT_ACTIVE_SHIFT;
    int k = toupper(key), f;
    (void)x; (void)y;

    for (f = 0; f < 6; f++)
        if (k == FACE_NAME[f]) { doTurn(f, shift ? -1 : 1); return; }

    switch (k) {
        case 'S': doScramble(); break;
        case 'Z': doUndo();     break;
        case 'V': doSolve();    break;
        case 'X': doReset();    break;
        case 'H': showKeys = !showKeys; glutPostRedisplay(); break;
        case 27:  exit(0);
    }
}

static void mouse(int button, int state, int x, int y)
{
    if (button == GLUT_LEFT_BUTTON) {
        dragging = (state == GLUT_DOWN);
        lastX = x; lastY = y;
    }
}

static void motion(int x, int y)
{
    if (!dragging) return;
    viewRotY += (x - lastX) * 0.5f;
    viewRotX += (y - lastY) * 0.5f;
    lastX = x; lastY = y;
    glutPostRedisplay();
}

/* Menu ids: 0..11 = face turns (face*2 + 0 clockwise / 1 counter). */
enum { M_SCRAMBLE = 100, M_UNDO, M_SOLVE, M_RESET, M_KEYS, M_QUIT };

static void menuFunc(int v)
{
    if (v < 12) { doTurn(v / 2, (v % 2) ? -1 : 1); return; }
    switch (v) {
        case M_SCRAMBLE: doScramble(); break;
        case M_UNDO:     doUndo();     break;
        case M_SOLVE:    doSolve();    break;
        case M_RESET:    doReset();    break;
        case M_KEYS:     showKeys = !showKeys; glutPostRedisplay(); break;
        case M_QUIT:     exit(0);
    }
}

static void createMenu(void)
{
    char label[32];
    int f, sub = glutCreateMenu(menuFunc);
    for (f = 0; f < 6; f++) {
        sprintf(label, "%c  (clockwise)", FACE_NAME[f]);
        glutAddMenuEntry(label, f * 2);
        sprintf(label, "%c' (counter-clockwise)", FACE_NAME[f]);
        glutAddMenuEntry(label, f * 2 + 1);
    }
    glutCreateMenu(menuFunc);
    glutAddSubMenu("Turn a face", sub);
    glutAddMenuEntry("Scramble (S)", M_SCRAMBLE);
    glutAddMenuEntry("Undo (Z)", M_UNDO);
    glutAddMenuEntry("Solve (V)", M_SOLVE);
    glutAddMenuEntry("Reset (X)", M_RESET);
    glutAddMenuEntry("Show or hide keys (H)", M_KEYS);
    glutAddMenuEntry("Quit (Esc)", M_QUIT);
    glutAttachMenu(GLUT_RIGHT_BUTTON);
}

static void initGL(void)
{
    GLfloat globalAmb[4] = { 0.30f, 0.31f, 0.33f, 1.0f };
    GLfloat keyDiff[4]   = { 0.95f, 0.94f, 0.92f, 1.0f };   /* main light */
    GLfloat keySpec[4]   = { 0.90f, 0.90f, 0.90f, 1.0f };
    GLfloat fillDiff[4]  = { 0.38f, 0.40f, 0.45f, 1.0f };   /* soft fill from the left */
    GLfloat noSpec[4]    = { 0.0f, 0.0f, 0.0f, 1.0f };

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, globalAmb);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);    /* nicer highlights */

    glEnable(GL_LIGHT0);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  keyDiff);
    glLightfv(GL_LIGHT0, GL_SPECULAR, keySpec);
    glEnable(GL_LIGHT1);
    glLightfv(GL_LIGHT1, GL_DIFFUSE,  fillDiff);
    glLightfv(GL_LIGHT1, GL_SPECULAR, noSpec);

    glEnable(GL_COLOR_MATERIAL);             /* glColor sets ambient+diffuse */
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_MULTISAMPLE);                /* anti-aliasing, if available */
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
}

/* ------------------------------------------------------------
 *  SELF-TEST  (run:  rubik --test)
 * ------------------------------------------------------------ */
#define CHECK(cond, msg) do { if (cond) printf("PASS  %s\n", msg); \
                              else { printf("FAIL  %s\n", msg); fails++; } } while (0)

static int runTests(void)
{
    int fails = 0, f, d, i, ok;
    Move m, seq[200];
    Cubie *c;

    /* 1. Any face turned 4 times returns to solved. */
    ok = 1;
    for (f = 0; f < 6; f++)
        for (d = -1; d <= 1; d += 2) {
            resetCube(); m.face = f; m.dir = d;
            for (i = 0; i < 4; i++) applyMove(m);
            if (!isSolved()) ok = 0;
        }
    CHECK(ok, "every face x4 returns to solved");

    /* 2. A move followed by its inverse returns to solved. */
    ok = 1;
    for (f = 0; f < 6; f++) {
        resetCube(); m.face = f; m.dir = 1;
        applyMove(m); applyMove(inverse(m));
        if (!isSolved()) ok = 0;
    }
    CHECK(ok, "move + inverse returns to solved");

    /* 3. Direction check against real-cube notation:
          R moves the front-right edge up, and its green sticker faces up. */
    resetCube(); m.face = 2; m.dir = 1; applyMove(m);
    c = &cube[homeIndex(1, 0, 1)];
    CHECK(c->pos[0] == 1 && c->pos[1] == 1 && c->pos[2] == 0, "R: front-right edge moves to up-right");
    CHECK(c->rot[0][2] == 0 && c->rot[1][2] == 1 && c->rot[2][2] == 0, "R: its green sticker now faces up");

    /* 4. U moves the up-front edge to the left; F moves it to the right. */
    resetCube(); m.face = 0; m.dir = 1; applyMove(m);
    c = &cube[homeIndex(0, 1, 1)];
    CHECK(c->pos[0] == -1 && c->pos[1] == 1 && c->pos[2] == 0, "U: up-front edge moves to up-left");
    resetCube(); m.face = 4; m.dir = 1; applyMove(m);
    c = &cube[homeIndex(0, 1, 1)];
    CHECK(c->pos[0] == 1 && c->pos[1] == 0 && c->pos[2] == 1, "F: up-front edge moves to front-right");

    /* 5. 200 random moves: positions stay valid and unique; reversing solves. */
    resetCube(); srand(12345); ok = 1;
    for (i = 0; i < 200; i++) {
        int occ[27] = {0}, j;
        seq[i].face = rand() % 6; seq[i].dir = (rand() % 2) ? 1 : -1;
        applyMove(seq[i]);
        for (j = 0; j < 27; j++) {
            int *p = cube[j].pos;
            if (p[0] < -1 || p[0] > 1 || p[1] < -1 || p[1] > 1 || p[2] < -1 || p[2] > 1) ok = 0;
            else if (occ[homeIndex(p[0], p[1], p[2])]++) ok = 0;
        }
    }
    CHECK(ok, "random moves keep 27 distinct valid positions");
    CHECK(!isSolved(), "cube is scrambled after random moves");
    for (i = 199; i >= 0; i--) applyMove(inverse(seq[i]));
    CHECK(isSolved(), "replaying inverses backwards solves the cube");

    printf("%s (%d failure%s)\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------
 *  MAIN
 * ------------------------------------------------------------ */
int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--test") == 0)
        return runTests();

    srand((unsigned)time(NULL));
    resetCube();

    glutInit(&argc, argv);
    /* Ask for anti-aliasing; fall back quietly if the GPU/driver can't. */
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH | GLUT_MULTISAMPLE);
    if (!glutGet(GLUT_DISPLAY_MODE_POSSIBLE))
        glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(winW, winH);
    glutCreateWindow("Rubik's Cube Simulator");

    initGL();
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutMouseFunc(mouse);
    glutMotionFunc(motion);
    createMenu();

    glutMainLoop();
    return 0;
}
