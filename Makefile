CXX      = g++
TARGET   = ai-desktop-companion.exe
SRCS     = src/main.cpp src/animation.cpp src/ledge.cpp src/flight.cpp src/sleep.cpp
OBJS     = $(SRCS:.cpp=.o)
INCLUDES = -I lib/json
CXXFLAGS = -std=c++17 $(INCLUDES)
LIBS     = -mwindows

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)
