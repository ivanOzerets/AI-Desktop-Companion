CXX = g++
TARGET = ai-desktop-companion.exe
SRC = src/main.cpp

INCLUDES = -I lib/json
LIBS = -mwindows

$(TARGET): $(SRC)
	$(CXX) $(SRC) -o $(TARGET) $(INCLUDES) $(LIBS) -std=c++17

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
