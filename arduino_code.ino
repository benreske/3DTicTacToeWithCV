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

uint8_t  currentPlayer  = PLAYER_ONE;
bool     gameOver       = false;
uint8_t  winner         = EMPTY;

// Button state
bool     lastButtonState = HIGH;   // INPUT_PULLUP → idle = HIGH
bool     buttonState = HIGH;  
uint32_t lastDebounceTime = 0;
bool     buttonPressed   = false;  // single-shot flag

bool winAnimBlackDone = false;
uint32_t winAnimBlackStart = 0;

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
    winAnimation();
    delay(20);

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

    return;
  }

  if (MODE == 0) {
    updateCursor();
    readButton();
    updateLEDs(true);

    if (buttonPressed) {
      buttonPressed = false;
      tryPlaceMove();
    }
  } else {
    updateLEDs(false);
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

        free(new_board);
        free(place_pos);

        currentPlayer = board[coords_ptr->z][coords_ptr->y][coords_ptr->x];
        move_animation(coords_ptr->z, coords_ptr->y, coords_ptr->x);

        process_win(currentPlayer);
      } else {
        perror("Wrong message");
      }
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

  move_animation(cursor.z, cursor.y, cursor.x);

  if (process_win(currentPlayer)) return;

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
    Serial.printf("*** Player %d wins! ***\n", winner);
    return true;
  }

  if (checkDraw()) {
    gameOver = true;
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

  cursor = {0, 0, 0};
  currentPlayer = PLAYER_ONE;
  gameOver = false;
  winner = EMPTY;

  winAnimBlackDone = false;
  winAnimBlackStart = 0;
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

  for (int z = 0; z < 3; z++) {
    for (int y = 0; y < 3; y++) {
      for (int x = 0; x < 3; x++) {
        if (board[z][y][x] == PLAYER_ONE) {
          drawNode(x, y, z, CRGB::Blue);
        } else if (board[z][y][x] == PLAYER_TWO) {
          drawNode(x, y, z, CRGB::Red);
        }
      }
    }
  }

  bool blinkOn = (millis() / 300) % 2;
  if (blinking && blinkOn) {
    double frac = ((double)(millis() % 300)) / 300.0;

    uint8_t cell = board[cursor.z][cursor.y][cursor.x];

    int color = (int)(255 * std::sin(frac * 3.1415926536));

    if (cell == EMPTY) {
      drawNode(cursor.x, cursor.y, cursor.z, CRGB(color, color, color));
    } else if (cell == PLAYER_ONE) {
      drawNode(cursor.x, cursor.y, cursor.z, CRGB(color, color, 255));
    } else if (cell == PLAYER_TWO) {
      drawNode(cursor.x, cursor.y, cursor.z, CRGB(255, color, color));
    }
  }

  FastLED.show();
}

const uint8_t X_COORDS[5][2] = {
  {0,0}, {2,0}, {1,1}, {0,2}, {2,2}
};

// Nodes that form an O on any layer (ring of 8, skip center)
const uint8_t O_COORDS[8][2] = {
  {0,0},{1,0},{2,0},
  {0,1},      {2,1},
  {0,2},{1,2},{2,2}
};

// Light a shape on all 3 layers (extruded upward)
void drawExtrudedShape(const uint8_t coords[][2], uint8_t count, CRGB color) {
  for (uint8_t z = 0; z < 3; z++) {
    for (uint8_t i = 0; i < count; i++) {
      drawNode(coords[i][0], coords[i][1], z, color);
    }
  }
}

void winAnimation() {
  if (!winAnimBlackDone) {
    FastLED.clear();
    FastLED.show();
    winAnimBlackStart = millis();
    winAnimBlackDone = true;
    return;
  }

  if (millis() - winAnimBlackStart < 1000) return;

  FastLED.clear();

  if (winner == PLAYER_ONE) {
    drawExtrudedShape(O_COORDS, 8, CRGB::Blue);
  } else if (winner == PLAYER_TWO) {
    drawExtrudedShape(X_COORDS, 5, CRGB::Red);
  }

  FastLED.show();
}

void restart() {
  currentPlayer  = PLAYER_ONE;
  gameOver       = false;
  winner         = EMPTY;

  lastButtonState = HIGH;
  buttonState = HIGH;  
  lastDebounceTime = 0;
  buttonPressed   = false;

  winAnimBlackDone = false;
  winAnimBlackStart = 0;

  resetBoard();
  start_animation();
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

void start_animation() {
  int steps = 50;
  for (int counter = 0; counter < steps; counter++) {
    double frac = (double)counter / (double)steps;
    double height = 255 * std::sin(frac * 3.1415926536);
    double deviation = 0.1 + 0.4 * std::sin(frac * 3.1415926536);

    for (int i = 0; i < BOARD_SIZE; i++) {
      double dist1 = (double)i - (double)(BOARD_SIZE / 2);
      for (int j = 0; j < BOARD_SIZE; j++) {
        double dist2 = (double)j - (double)(BOARD_SIZE / 2);
        for (int k = 0; k < BOARD_SIZE; k++) {
          double dist3 = (double)k - (double)(BOARD_SIZE / 2);

          double dist = std::pow(dist1 * dist1 + dist2 * dist2 + dist3 * dist3, 0.5);
          int color;
          if (dist < 0.1) {
            color = std::min(static_cast<int>(height), 255);
          } else {
            double exp = -((std::pow(dist - 0.0, 2)) / (2 * std::pow(deviation, 2)));
            double result = height * std::pow(2.7182818284, exp);
            color = std::min(static_cast<int>(result), 255);
          }
          drawNode(k, j, i, CRGB(0, color, 0));
        }
      }
    }

    FastLED.show();

    delay(20);
  }
}

void move_animation(uint8_t pos_z, uint8_t pos_y, uint8_t pos_x) {
  int steps = 50;
  for (int counter = 0; counter < steps; counter++) {
    double frac = (double)counter / (double)steps;
    double radius = 3.0 * (1.0 - frac);
    double height = 127.0 * (1.0 + frac);
    double deviation = 0.2 * (1.25 - frac);

    for (int i = 0; i < BOARD_SIZE; i++) {
      double dist1 = (double)i - (double)pos_z;
      for (int j = 0; j < BOARD_SIZE; j++) {
        double dist2 = (double)j - (double)pos_y;
        for (int k = 0; k < BOARD_SIZE; k++) {
          double dist3 = (double)k - (double)pos_x;

          double dist = std::pow(dist1 * dist1 + dist2 * dist2 + dist3 * dist3, 0.5);
          double rel_dist = std::abs(dist - radius);

          int color;
          if (rel_dist < 0.1) {
            color = std::min(static_cast<int>(height), 255);
          } else {
            double exp = -((std::pow(rel_dist - 0.0, 2)) / ((2 * std::pow(deviation, 2))));
            double result = height * std::pow(2.7182818284, exp);
            color = std::min(static_cast<int>(result), 255);
          }
          if (currentPlayer == PLAYER_ONE) {
            if (board[i][j][k] == PLAYER_ONE) {
              drawNode(k, j, i, CRGB(0, 0, 255));
            } else if (board[i][j][k] == PLAYER_TWO) {
              drawNode(k, j, i, CRGB(255, 0, color));
            } else {
              drawNode(k, j, i, CRGB(0, 0, color));
            }
          } else if (currentPlayer == PLAYER_TWO) {
            if (board[i][j][k] == PLAYER_ONE) {
              drawNode(k, j, i, CRGB(color, 0, 255));
            } else if (board[i][j][k] == PLAYER_TWO) {
              drawNode(k, j, i, CRGB(255, 0, 0));
            } else {
              drawNode(k, j, i, CRGB(color, 0, 0));
            }
          } else {
            perror("Neither player is playing?");
          }
        }
      }
    }

    FastLED.show();

    delay(20);
  }
}
