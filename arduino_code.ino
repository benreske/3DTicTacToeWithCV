// ============================================================
//  3x3x3 LED Matrix Tic-Tac-Toe  —  ESP32
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>

// ---- Pin Definitions ----------------------------------------
#define POT_X_PIN   34   // ADC1 — X axis potentiometer
#define POT_Y_PIN   35   // ADC1 — Y axis potentiometer
#define POT_Z_PIN   32   // ADC1 — Z axis potentiometer (layer)
#define BUTTON_PIN  21   // Commit move button (INPUT_PULLUP)

// Modes: 0 is manual, 1 is with CV
#define MODE        1

// ---- Board Constants ----------------------------------------
#define BOARD_SIZE     3
#define OPTION_CAP     (BOARD_SIZE + 1)
#define BOARD_CELLS    (BOARD_SIZE * BOARD_SIZE * BOARD_SIZE)
#define EMPTY          0
#define PLAYER_ONE     1
#define PLAYER_TWO     2

// ---- ADC Tuning ---------------------------------------------
// ESP32 ADC is 12-bit (0–4095). Each third = ~1365 counts.
// Deadband keeps cursor from flickering at zone edges.
#define ADC_MAX        4095
#define DEADBAND        200   // counts either side of boundary

// Zone boundaries (with deadband baked in)
#define ZONE_LOW_MID  1500  // Started at 1365
#define ZONE_LOW_HI   (ZONE_LOW_MID - DEADBAND)
#define ZONE_LOW_LO   (ZONE_LOW_MID + DEADBAND)
#define ZONE_MID_MID  3000  // Started at 2730
#define ZONE_MID_HI   (ZONE_MID_MID - DEADBAND)
#define ZONE_MID_LO   (ZONE_MID_MID + DEADBAND)

#define BLINK_DURATION  800

// ---- Button Debounce ----------------------------------------
#define DEBOUNCE_MS   50

// ---- Global State -------------------------------------------

#define NUM_LEDS 135
CRGB leds[NUM_LEDS];

// The board: board[z][y][x]
// z = layer (0=bottom, 2=top)
// y = row,  x = column
uint8_t board[BOARD_SIZE][BOARD_SIZE][BOARD_SIZE];

// Cursor position (the cell currently selected by the pots)
struct Cursor {
  uint8_t x, y, z;
} cursor;

// Animation struct
typedef void ani_fn(int duration, int progress, int args[256]);
struct Animation {
  int duration;
  int progress;
  ani_fn *animation;
  bool on;
  int args[256];
};

Animation start_ani_head;
Animation *start_ani = &start_ani_head;

Animation move_ani_head;
Animation *move_ani = &move_ani_head;

Animation begin_win_ani_head;
Animation *begin_win_ani = &begin_win_ani_head;

Animation continue_win_ani_head;
Animation *continue_win_ani = &continue_win_ani_head;

Animation static_ani_head;
Animation *static_ani = &static_ani_head;

// Animation ambient_ani_head;
// Animation *ambient_ani = &ambient_ani_head;

uint8_t  currentPlayer  = PLAYER_ONE;
bool     gameOver       = false;
uint8_t  winner         = EMPTY;

// Button state
bool     lastButtonState = HIGH;   // INPUT_PULLUP → idle = HIGH
bool     buttonState = HIGH;  
uint32_t lastDebounceTime = 0;
bool     buttonPressed   = false;  // single-shot flag

uint8_t lit_level = OPTION_CAP;
uint8_t lit_node[3] = {OPTION_CAP, OPTION_CAP, OPTION_CAP};

// LED mapping: nodeIndex[z][y][x] = first LED index in strip
// Each node occupies firstLED through firstLED+4
const uint8_t nodeIndex[3][3][3] = {
  // Z=0 (bottom) — row snakes left→right→left→right
  {
    { 0,  5, 10},  // y=0: left→right
    {25, 20, 15},  // y=1: right→left
    {30, 35, 40},  // y=2: left→right
  },
  // Z=1 (middle) — layer flipped, row snakes right→left→right→left
  {
    {85, 80, 75},  // y=0: right→left
    {60, 65, 70},  // y=1: left→right
    {55, 50, 45},  // y=2: right→left
  },
  // Z=2 (top) — layer flipped back, same as Z=0
  {
    { 90,  115, 120},  // y=0: left→right
    {95, 110, 125},  // y=1: right→left
    {100, 105, 130},  // y=2: left→right
  }
};

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin();
  FastLED.addLeds<WS2812, 13, GRB>(leds, NUM_LEDS);

  // ADC pins are input by default on ESP32; no pinMode needed.
  // Optional: set ADC attenuation for full 0–3.3 V range.
  analogSetAttenuation(ADC_11db);

  // Set up animations
  start_ani->duration = 50;
  start_ani->progress = 0;
  start_ani->animation = &start_animation_fn;
  start_ani->on = false;
  start_ani->args[0] = 1000; start_ani->args[1] = 400; start_ani->args[2] = 0;
  start_ani->args[3] = 255; start_ani->args[4] = 0;

  move_ani->duration = 50;
  move_ani->progress = 0;
  move_ani->animation = &move_animation_fn;
  move_ani->on = false;
  move_ani->args[0] = 500; move_ani->args[1] = 200; move_ani->args[2] = 3000;
  move_ani->args[3] = 0; move_ani->args[4] = 0; move_ani->args[5] = 0;
  move_ani->args[6] = 0; move_ani->args[7] = 0; move_ani->args[8] = 0;

  begin_win_ani->duration = 50;
  begin_win_ani->progress = 0;
  begin_win_ani->animation = &begin_win_animation_fn;
  begin_win_ani->on = false;
  begin_win_ani->args[0] = 250;
  begin_win_ani->args[1] = 0; begin_win_ani->args[2] = 0; begin_win_ani->args[3] = 0;

  continue_win_ani->duration = 1;
  continue_win_ani->progress = 0;
  continue_win_ani->animation = &continue_win_animation_fn;
  continue_win_ani->on = false;
  continue_win_ani->args[0] = 1000; continue_win_ani->args[1] = 300; continue_win_ani->args[2] = 50; continue_win_ani->args[3] = 2500; continue_win_ani->args[4] = 0;
  continue_win_ani->args[5] = 0; continue_win_ani->args[6] = 0; continue_win_ani->args[7] = 0;
  
  static_ani->duration = 1;
  static_ani->progress = 0;
  static_ani->animation = &static_animation_fn;
  static_ani->on = true;

  // ambient_ani->duration = 50;
  // ambient_ani->progress = 0;
  // ambient_ani->animation = &ambient_animation_fn;
  // ambient_ani->on = true;
  // ambient_ani->args[0] = 1000;

  restart();

  Serial.println("3x3x3 Tic-Tac-Toe ready!");
  Serial.println("Player 1's turn.");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  if (MODE == 1) {
    readButton();
    if (buttonPressed) {
      buttonPressed = false;
      restart();
    }
  }

  if (gameOver) {
    if (Serial.available() > 0) {
      char first_byte = Serial.read();
      if (first_byte == 1) {
        restart();
      } else {
        perror("Wrong message");
      }
    }

    readButton();
    if (buttonPressed) {
      buttonPressed = false;
      restart();
    }

    if ((double)begin_win_ani->progress / (double)begin_win_ani->duration > 0.5) {
      if (!continue_win_ani->on) {
        launch_continue_win();
      }
    }

    updateLEDs(false);
  } else {
    if (MODE == 0) {
      updateCursor();
      readButton();

      if (buttonPressed) {
        buttonPressed = false;
        tryPlaceMove();
      }
      updateLEDs(true);
    } else {
      readButton();
      if (buttonPressed) {
        buttonPressed = false;
        restart();
      }

      if (Serial.available() > 0) {
        char first_byte = Serial.read();
        if (first_byte == 0) {
          char *place_pos = new char[3]();
          Serial.readBytes(place_pos, 3);
          Cursor coords = {place_pos[2], place_pos[1], place_pos[0]};
          Cursor *coords_ptr = &coords;
          rotate_coords(coords_ptr, 0);


          char *new_board = new char[BOARD_CELLS]();
          Serial.readBytes(new_board, BOARD_CELLS);
          make_board_from_flat(new_board);

          rotate_board(0);

          lit_level = OPTION_CAP;
          lit_node[0] = OPTION_CAP;
          lit_node[1] = OPTION_CAP;
          lit_node[2] = OPTION_CAP;

          free(new_board);
          free(place_pos);

          currentPlayer = board[coords_ptr->z][coords_ptr->y][coords_ptr->x];

          if (process_win(currentPlayer)) {
            launch_move(coords_ptr->z, coords_ptr->y, coords_ptr->x);
          }
        } else if (first_byte == 1) {
          restart();
        } else if (first_byte == 2) {
          lit_level = Serial.read();
        } else if (first_byte == 3) {
          char *place_pos = new char[3]();
          Serial.readBytes(place_pos, 3);
          Cursor coords = {place_pos[2], place_pos[1], place_pos[0]};
          Cursor *coords_ptr = &coords;
          rotate_coords(coords_ptr, 0);

          lit_node[0] = coords_ptr->z;
          lit_node[1] = coords_ptr->y;
          lit_node[2] = coords_ptr->x;

          free(place_pos);
          
        } else {
          perror("Wrong message");
        }
      }
      updateLEDs(false);
    }
  }

  // TODO: call your LED update function here, passing board[] and cursor
  delay(20);  // ~50 Hz loop
}

// ============================================================
//  Read one potentiometer and map to 0, 1, or 2
//  Uses hysteresis: if the reading lands in the deadband, the
//  axis keeps its previous value (passed in as `prev`).
// ============================================================
uint8_t potToAxis(int pin, uint8_t prev) {
  int raw = analogRead(pin);

  if      (raw <= ZONE_LOW_HI)  return 0;
  else if (raw >= ZONE_LOW_LO && raw <= ZONE_MID_HI) return 1;
  else if (raw >= ZONE_MID_LO)  return 2;
  else                          return prev;  // in deadband — hold
}

// ============================================================
//  Update cursor from potentiometers
// ============================================================
void updateCursor() {
  uint8_t newX = potToAxis(POT_X_PIN, cursor.x);
  uint8_t newY = potToAxis(POT_Y_PIN, cursor.y);
  uint8_t newZ = potToAxis(POT_Z_PIN, cursor.z);

  if (newX != cursor.x || newY != cursor.y || newZ != cursor.z) {
    cursor.x = newX;
    cursor.y = newY;
    cursor.z = newZ;

    Serial.printf("Cursor → X:%d Y:%d Z:%d\n", cursor.x, cursor.y, cursor.z);
  }
}

// ============================================================
//  Button reading with software debounce
// ============================================================

void readButton() {
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
    lastButtonState = reading;
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
    if (reading == LOW && buttonState == HIGH) {  // falling edge, debounced
      buttonPressed = true;
    }
    buttonState = reading;
  }
}

// ============================================================
//  Attempt to place the current player's piece at cursor
// ============================================================
void tryPlaceMove() {
  if (board[cursor.z][cursor.y][cursor.x] != EMPTY) {
    Serial.println("Cell occupied! Choose another.");
    return;
  }

  board[cursor.z][cursor.y][cursor.x] = currentPlayer;
  Serial.printf("Player %d placed at X:%d Y:%d Z:%d\n",
               currentPlayer, cursor.x, cursor.y, cursor.z);

  printBoard();

  if (process_win(currentPlayer)) return;

  launch_move(cursor.z, cursor.y, cursor.x);

  currentPlayer = (currentPlayer == PLAYER_ONE) ? PLAYER_TWO : PLAYER_ONE;
  Serial.printf("Player %d's turn.\n", currentPlayer);
}

// ============================================================
//  Win Detection
//  Checks all 49 winning lines in a 3x3x3 board:
//    - 9 rows (along X, one per Y/Z combo)
//    - 9 columns (along Y)
//    - 9 pillars (along Z)
//    - 18 face diagonals (2 per face × 3 axes)
//    - 4 space diagonals
// ============================================================
bool checkLine(uint8_t p,
               uint8_t x0,uint8_t y0,uint8_t z0,
               uint8_t x1,uint8_t y1,uint8_t z1,
               uint8_t x2,uint8_t y2,uint8_t z2) {
  return board[z0][y0][x0] == p &&
         board[z1][y1][x1] == p &&
         board[z2][y2][x2] == p;
}

bool checkWin(uint8_t p) {
  // --- Axis-aligned lines ---
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      // Rows along X (fixed y=i, z=j)
      if (checkLine(p, 0,i,j, 1,i,j, 2,i,j)) return true;
      // Columns along Y (fixed x=i, z=j)
      if (checkLine(p, i,0,j, i,1,j, i,2,j)) return true;
      // Pillars along Z (fixed x=i, y=j)
      if (checkLine(p, i,j,0, i,j,1, i,j,2)) return true;
    }
  }

  // --- Face diagonals ---
  // XY-plane faces (fixed z)
  for (int z = 0; z < 3; z++) {
    if (checkLine(p, 0,0,z, 1,1,z, 2,2,z)) return true;
    if (checkLine(p, 2,0,z, 1,1,z, 0,2,z)) return true;
  }
  // XZ-plane faces (fixed y)
  for (int y = 0; y < 3; y++) {
    if (checkLine(p, 0,y,0, 1,y,1, 2,y,2)) return true;
    if (checkLine(p, 2,y,0, 1,y,1, 0,y,2)) return true;
  }
  // YZ-plane faces (fixed x)
  for (int x = 0; x < 3; x++) {
    if (checkLine(p, x,0,0, x,1,1, x,2,2)) return true;
    if (checkLine(p, x,2,0, x,1,1, x,0,2)) return true;
  }

  // --- Space diagonals (4 main diagonals through centre) ---
  if (checkLine(p, 0,0,0, 1,1,1, 2,2,2)) return true;
  if (checkLine(p, 2,0,0, 1,1,1, 0,2,2)) return true;
  if (checkLine(p, 0,2,0, 1,1,1, 2,0,2)) return true;
  if (checkLine(p, 2,2,0, 1,1,1, 0,0,2)) return true;

  return false;
}

bool process_win(uint8_t p) {
  if (checkWin(currentPlayer)) {
    winner = currentPlayer;
    gameOver = true;
    launch_begin_win();
    static_ani->on = false;
    Serial.printf("*** Player %d wins! ***\n", winner);
    return true;
  }

  if (checkDraw()) {
    gameOver = true;
    launch_begin_win();
    static_ani->on = false;
    Serial.println("*** It's a draw! ***");
    return true;
  }
  return false;
}

// ============================================================
//  Draw Detection — all 27 cells filled with no winner
// ============================================================
bool checkDraw() {
  for (int z = 0; z < BOARD_SIZE; z++)
    for (int y = 0; y < BOARD_SIZE; y++)
      for (int x = 0; x < BOARD_SIZE; x++)
        if (board[z][y][x] == EMPTY) return false;
  return true;
}

// ============================================================
//  Reset board to all EMPTY
// ============================================================
void resetBoard() {
  for (int z = 0; z < BOARD_SIZE; z++)
    for (int y = 0; y < BOARD_SIZE; y++)
      for (int x = 0; x < BOARD_SIZE; x++)
        board[z][y][x] = EMPTY;
}

// ============================================================
//  Debug: print all 3 layers to Serial Monitor
// ============================================================
void printBoard() {
  Serial.println("--- Board State ---");
  for (int z = 2; z >= 0; z--) {   // print top layer first
    Serial.printf("Layer Z=%d:\n", z);
    for (int y = 0; y < BOARD_SIZE; y++) {
      for (int x = 0; x < BOARD_SIZE; x++) {
        // char c = '.';
        // if      (board[z][y][x] == PLAYER_ONE) c = '1';
        // else if (board[z][y][x] == PLAYER_TWO) c = '2';
        // Serial.printf("%c ", c);
        Serial.printf("%d ", int(board[z][y][x]));
      }
      Serial.println();
    }
  }
  Serial.println("-------------------");
}

// Set all 5 LEDs of a node to a color
void drawNode(uint8_t x, uint8_t y, uint8_t z, CRGB color) {
  uint8_t first = nodeIndex[z][y][x];
  for (uint8_t i = 0; i < 5; i++) {
    leds[first + i] = color;
  }
}

int color_combine(int c1, int c2) {
  return std::max(c1, c2);
  // return std::pow(c1 ** 2 + c2 ** 2, 0.5);
}

void draw_node_combine(uint8_t x, uint8_t y, uint8_t z, CRGB color) {
  uint8_t first = nodeIndex[z][y][x];
  for (uint8_t i = 0; i < 5; i++) {
    int r = color_combine(leds[first + i].r, color.r);
    int g = color_combine(leds[first + i].g, color.g);
    int b = color_combine(leds[first + i].b, color.b);
    leds[first + i] = CRGB(r, g, b);
  }
}

void make_board_from_flat(char *flat) {
  int counter = 0;
  for (int z = 0; z < 3; z++) {
    for (int y = 0; y < 3; y++) {
      for (int x = 0; x < 3; x++) {
        board[z][y][x] = uint8_t(flat[counter]);
        counter += 1;
      }
    }
  }
}

void updateLEDs(bool blinking) {
  FastLED.clear();

  if (blinking || lit_node[0] < OPTION_CAP || lit_level < OPTION_CAP) {
    double frac = ((double)(millis() % BLINK_DURATION)) / BLINK_DURATION;
    int color = (int)(15 * std::sin(frac * 3.1415926536));
    if (blinking) {
      uint8_t cell = board[cursor.z][cursor.y][cursor.x];
      draw_node_combine(cursor.x, cursor.y, cursor.z, CRGB(color, color, color));

    } else if (lit_node[0] < OPTION_CAP) {
      uint8_t cell = board[lit_node[0]][lit_node[1]][lit_node[2]];
      draw_node_combine(lit_node[2], lit_node[1], lit_node[0], CRGB(color, color, color));

    } else if (lit_level < OPTION_CAP) {
      for (int j = 0; j < BOARD_SIZE; j++) {
        for (int k = 0; k < BOARD_SIZE; k++) {
          uint8_t cell = board[lit_level][j][k];
          draw_node_combine(k, j, lit_level, CRGB(color, color, color));
        }
      }
    }
  }

  if (start_ani->on) {
    (*start_ani->animation)(start_ani->duration, start_ani->progress, start_ani->args);
    update_animation(start_ani, false);
  }

  if (static_ani->on) {
    (*static_ani->animation)(static_ani->duration, static_ani->progress, static_ani->args);
    update_animation(static_ani, true);
  }

  if (move_ani->on) {
    (*move_ani->animation)(move_ani->duration, move_ani->progress, move_ani->args);
    update_animation(move_ani, false);
  }

  if (begin_win_ani->on) {
    (*begin_win_ani->animation)(begin_win_ani->duration, begin_win_ani->progress, begin_win_ani->args);
    update_animation(begin_win_ani, false);
  }

  if (continue_win_ani->on) {
    (*continue_win_ani->animation)(continue_win_ani->duration, continue_win_ani->progress, continue_win_ani->args);
    update_animation(continue_win_ani, true);
  }

  // if (ambient_ani->on) {
  //   (*ambient_ani->animation)(ambient_ani->duration, ambient_ani->progress, ambient_ani->args);
  //   update_animation(ambient_ani, true);
  // }

  FastLED.show();
}

void restart() {
  cursor = {0, 0, 0};
  currentPlayer  = PLAYER_ONE;
  gameOver       = false;
  winner         = EMPTY;

  lastButtonState = HIGH;
  buttonState = HIGH;  
  lastDebounceTime = 0;
  buttonPressed   = false;

  lit_level = OPTION_CAP;
  lit_node[0] = OPTION_CAP;
  lit_node[1] = OPTION_CAP;
  lit_node[2] = OPTION_CAP;

  resetBoard();

  launch_static();
  launch_start();
  // launch_ambient();
  continue_win_ani->on = false;
}

void rotate_coords(Cursor *coords, int axis) {
  char offset = BOARD_SIZE / 2;
  if (axis == 0) {
    char old_x = (char)coords->x - offset;
    char old_y = (char)coords->y - offset;
    coords->x = (uint8_t)(-old_y + offset);
    coords->y = (uint8_t)(old_x + offset);
  } else if (axis == 1) {
    char old_x = (char)coords->x - offset;
    char old_z = (char)coords->z - offset;
    coords->x = (uint8_t)(-old_z + offset);
    coords->z = (uint8_t)(old_x + offset);
  } else if (axis == 2) {
    char old_y = (char)coords->y - offset;
    char old_z = (char)coords->z - offset;
    coords->y = (uint8_t)(-old_z + offset);
    coords->z = (uint8_t)(old_y + offset);
  } else perror("Invalid axis");
}

void rotate_board(int axis) {
  uint8_t copied[BOARD_SIZE][BOARD_SIZE][BOARD_SIZE];
  for (int i = 0; i < BOARD_SIZE; i++) {
    for (int j = 0; j < BOARD_SIZE; j++) {
      for (int k = 0; k < BOARD_SIZE; k++) {
        copied[i][j][k] = board[i][j][k];
      }
    }
  }
  for (int i = 0; i < BOARD_SIZE; i++) {
    for (int j = 0; j < BOARD_SIZE; j++) {
      for (int k = 0; k < BOARD_SIZE; k++) {
        Cursor coords = {k, j, i};
        Cursor *coord_ptr = &coords;

        rotate_coords(coord_ptr, axis);
        board[coord_ptr->z][coord_ptr->y][coord_ptr->x] = copied[i][j][k];
      }
    }
  }
}

void update_animation(Animation *anima, bool loop) {
  if (anima->on) {
    anima->progress += 1;
    if (anima->progress >= anima->duration && !loop) {
      anima->progress = 0;
      anima->on = false;
    }
  }
}

// args here are: [height * 1000, deviation * 1000, peak_color_r, peak_color_g, peak_color_b]
void start_animation_fn(int duration, int progress, int args[256]) {
  double frac = (double)progress / (double)duration;
  double height = ((double)args[0] / 1000) * std::sin(frac * 3.1415926536);
  double deviation = 0.1 + ((double)args[1] / 1000) * std::sin(frac * 3.1415926536);

  for (int i = 0; i < BOARD_SIZE; i++) {
    double dist1 = (double)i - (double)(BOARD_SIZE / 2);
    for (int j = 0; j < BOARD_SIZE; j++) {
      double dist2 = (double)j - (double)(BOARD_SIZE / 2);
      for (int k = 0; k < BOARD_SIZE; k++) {
        double dist3 = (double)k - (double)(BOARD_SIZE / 2);

        double dist = std::pow(dist1 * dist1 + dist2 * dist2 + dist3 * dist3, 0.5);
        double color_scale;
        if (dist < 0.1) {
          color_scale = height;
        } else {
          double exp = -((std::pow(dist - 0.0, 2)) / (2 * std::pow(deviation, 2)));
          double result = height * std::pow(2.7182818284, exp);
          color_scale = result;
        }
        int r_color = std::min(static_cast<int>(color_scale * args[2]), 255);
        int g_color = std::min(static_cast<int>(color_scale * args[3]), 255);
        int b_color = std::min(static_cast<int>(color_scale * args[4]), 255);
        draw_node_combine(k, j, i, CRGB(r_color, g_color, b_color));
      }
    }
  }
}

void launch_start() {
  start_ani->progress = 0;
  start_ani->on = true;
}

// args here are: []
void static_animation_fn(int duration, int progress, int args[256]) {
  for (int z = 0; z < 3; z++) {
    for (int y = 0; y < 3; y++) {
      for (int x = 0; x < 3; x++) {
        if (board[z][y][x] == PLAYER_ONE) {
          draw_node_combine(x, y, z, CRGB(0, 0, 255));
        } else if (board[z][y][x] == PLAYER_TWO) {
          draw_node_combine(x, y, z, CRGB(255, 0, 0));
        }
      }
    }
  }
}

void launch_static() {
  static_ani->progress = 0;
  static_ani->on = true;
}

// args here are: [height * 1000, deviation * 1000, radius * 1000, pos_z, pos_y, pos_x, peak_color_r, peak_color_g, peak_color_b]
void move_animation_fn(int duration, int progress, int args[256]) {
  double frac = (double)progress / (double)duration;
  double height = ((double)args[0] / 1000) * (1.0 + frac);
  double deviation = ((double)args[1] / 1000) * (1.25 - frac);
  double radius = ((double)args[2] / 1000) * (1.0 - frac);

  for (int i = 0; i < BOARD_SIZE; i++) {
    double dist1 = (double)i - (double)args[3];
    for (int j = 0; j < BOARD_SIZE; j++) {
      double dist2 = (double)j - (double)args[4];
      for (int k = 0; k < BOARD_SIZE; k++) {
        double dist3 = (double)k - (double)args[5];

        double dist = std::pow(dist1 * dist1 + dist2 * dist2 + dist3 * dist3, 0.5);
        double rel_dist = std::abs(dist - radius);

        double color_scale;
        if (rel_dist < 0.1) {
          color_scale = height;
        } else {
          double exp = -((std::pow(rel_dist - 0.0, 2)) / ((2 * std::pow(deviation, 2))));
          double result = height * std::pow(2.7182818284, exp);
          color_scale = result;
        }
        int r_color = std::min(static_cast<int>(color_scale * args[6]), 255);
        int g_color = std::min(static_cast<int>(color_scale * args[7]), 255);
        int b_color = std::min(static_cast<int>(color_scale * args[8]), 255);

        draw_node_combine(k, j, i, CRGB(r_color, g_color, b_color));
      }
    }
  }
}

void launch_move(int pos_z, int pos_y, int pos_x) {
  move_ani->args[3] = pos_z; move_ani->args[4] = pos_y; move_ani->args[5] = pos_x;
  if (currentPlayer == PLAYER_ONE) {
    move_ani->args[6] = 0; move_ani->args[7] = 0; move_ani->args[8] = 255;
  } else if (currentPlayer == PLAYER_TWO) {
    move_ani->args[6] = 255; move_ani->args[7] = 0; move_ani->args[8] = 0;
  } else {
    perror("Neither player is playing?");
  }
  move_ani->progress = 0;
  move_ani->on = true;
}

// args here are: [height * 1000, peak_color_r, peak_color_g, peak_color_b]
void begin_win_animation_fn(int duration, int progress, int args[256]) {
  double frac = (double)progress / (double)duration;
  double height = ((double)args[0] / 1000) * std::sin(frac * 3.1415926536);

  for (int i = 0; i < BOARD_SIZE; i++) {
    for (int j = 0; j < BOARD_SIZE; j++) {
      for (int k = 0; k < BOARD_SIZE; k++) {
        double color_scale = height;
        int r_color = std::min(static_cast<int>(color_scale * args[1]), 255);
        int g_color = std::min(static_cast<int>(color_scale * args[2]), 255);
        int b_color = std::min(static_cast<int>(color_scale * args[3]), 255);

        draw_node_combine(k, j, i, CRGB(r_color, g_color, b_color));
      }
    }
  }
}

void launch_begin_win() {
  if (currentPlayer == PLAYER_ONE) {
    begin_win_ani->args[1] = 0; begin_win_ani->args[2] = 0; begin_win_ani->args[3] = 255;
  } else if (currentPlayer == PLAYER_TWO) {
    begin_win_ani->args[1] = 255; begin_win_ani->args[2] = 0; begin_win_ani->args[3] = 0;
  } else {
    perror("Neither player is playing?");
  }
  begin_win_ani->progress = 0;
  begin_win_ani->on = true;
}

bool in_x_coords(int z, int y, int x) {
  return ((x + y) % 2 == 0);
}

bool in_o_coords(int z, int y, int x) {
  return (x != 1 || y != 1);
}

// args here are: [height * 1000, deviation * 1000, speed * 1000, separation * 1000, shape ('x' or 'o'), peak_color_r, peak_color_g, peak_color_b]
void continue_win_animation_fn(int duration, int progress, int args[256]) {
  double frac = (double)progress / (double)duration;
  double height = ((double)args[0] / 1000);
  double deviation = (double)args[1] / 1000;
  double speed = (double)args[2] / 1000;
  double separation = (double)args[3] / 1000;
  char shape = (char)args[4];

  // double level = (BOARD_SIZE / 2) + frac * speed * (BOARD_SIZE - 1);
  double level = (BOARD_SIZE / 2) + frac * 0.1 * (BOARD_SIZE - 1);
  while (level > BOARD_SIZE - 1) level -= separation;

  for (int i = 0; i < BOARD_SIZE; i++) {
    double dist_z1 = std::abs(i - (level - separation / 2));
    double dist_z2 = std::abs(i - (level + separation / 2));
    double min_dist_z = std::min(dist_z1, dist_z2);
    for (int j = 0; j < BOARD_SIZE; j++) {
      for (int k = 0; k < BOARD_SIZE; k++) {

        double color_scale;
        if (min_dist_z < 0.1) {
          color_scale = height;
        } else {
          double exp = -((std::pow(min_dist_z - 0.0, 2)) / (2 * std::pow(deviation, 2)));
          double result = height * std::pow(2.7182818284, exp);
          color_scale = result;
        }
        int r_color = std::min(static_cast<int>(color_scale * args[5]), 255);
        int g_color = std::min(static_cast<int>(color_scale * args[6]), 255);
        int b_color = std::min(static_cast<int>(color_scale * args[7]), 255);

        if ((shape == 'x' && in_x_coords(i, j, k)) || (shape == 'o' && in_o_coords(i, j, k))) {
          draw_node_combine(k, j, i, CRGB(r_color, g_color, b_color));
        }
      }
    }
  }
}

void launch_continue_win() {
  if (currentPlayer == PLAYER_ONE) {
    continue_win_ani->args[4] = (int)'o';
    continue_win_ani->args[5] = 0; continue_win_ani->args[6] = 0; continue_win_ani->args[7] = 255;
  } else if (currentPlayer == PLAYER_TWO) {
    continue_win_ani->args[4] = (int)'x';
    continue_win_ani->args[5] = 255; continue_win_ani->args[6] = 0; continue_win_ani->args[7] = 0;
  } else {
    perror("Neither player is playing?");
  }
  continue_win_ani->progress = 0;
  continue_win_ani->on = true;
}

struct Segment {
  double center;
  double width;
  double speed;
  double height;
  double r_color;
  double g_color;
  double b_color;
};
double default_height = 0.03125;
Segment segment0 = {0, 10, 10, default_height, 255, 0, 0};
Segment segment1 = {37, 20, 25, default_height, 0, 255, 0};
Segment segment2 = {74, 30, 30, default_height, 0, 0, 255};
Segment segment3 = {111, 5, 5, default_height, 127, 127, 0};
Segment segment4 = {148, 15, 15, default_height, 0, 127, 127};
Segment segment5 = {185, 25, 7, default_height, 127, 0, 127};
Segment segment6 = {222, 10, 22, default_height, 63, 63, 63};
Segment segment7 = {259, 5, 13, default_height, 127, 255, 0};

int segment_number = 8;
Segment segment_list[8] = {segment0,
                           segment1,
                           segment2,
                           segment3,
                           segment4,
                           segment5,
                           segment6,
                           segment7};

// // args here are: [buildup * 1000]
// void ambient_animation_fn(int duration, int progress, int args[256]) {
//   double frac = (double)progress / (double)duration;

//   double buildup = args[0] / 1000;
//   double buildup_frac = frac / (double)buildup;
//   double height_multiplier = std::min(frac, 1.0);

//   for (int seg_i = 0; seg_i < segment_number; seg_i++) {
//     Segment this_segment = segment_list[seg_i];

//     int stream_center = this_segment.center + this_segment.speed * frac;
//     int stream_start = (int)(stream_center - this_segment.width / 2) + 1;
//     int stream_end = (int)(stream_center + this_segment.width / 2);
//     for (int i = stream_start; i <= stream_end; i++) {
//       double cos_height = std::cos(((i - stream_center) / this_segment.width) * 3.1415926536);
      
//       double color_scale = this_segment.height * height_multiplier * cos_height;

//       int r_color = std::min(static_cast<int>(color_scale * this_segment.r_color), 255);
//       int g_color = std::min(static_cast<int>(color_scale * this_segment.g_color), 255);
//       int b_color = std::min(static_cast<int>(color_scale * this_segment.b_color), 255);

//       int true_index = i % NUM_LEDS;

//       leds[true_index] = CRGB(color_combine(leds[true_index].r, r_color),
//                         color_combine(leds[true_index].g, g_color),
//                         color_combine(leds[true_index].b, b_color));
//     }
//   }

// }

// void launch_ambient() {
//   ambient_ani->progress = 0;
//   ambient_ani->on = true;
// }
