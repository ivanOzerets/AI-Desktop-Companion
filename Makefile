CXX = g++
TARGET = ai-desktop-companion.exe
SRC = src/main.cpp

INCLUDES = -I lib/SDL2/include -I lib/json
LIBS = -L lib/SDL2/lib -lSDL2 -lSDL2_image -lSDL2main -mwindows

$(TARGET): $(SRC)
	$(CXX) $(SRC) -o $(TARGET) $(INCLUDES) $(LIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
