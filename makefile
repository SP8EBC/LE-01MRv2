OBJS := main.o raw_mode.o serial_io_raw.o ./wjwwood_serial/src/serial.o ./wjwwood_serial/src/impl/unix.o
DEFINES := -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
INCLUDES := -I/usr/include/modbus -I/usr/include/spdlog -I ./wjwwood_serial/include -I.
LIBS := -lmodbus -lfmt -lboost_program_options 
LIBS_DIRS := -L/lib/x86_64-linux-gnu -L/lib/i386-linux-gnu -L/usr/lib
WARNS := -Wmaybe-uninitialized -Wpedantic -Wno-return-local-addr -Wparentheses


all: le-01mr

clean:
	rm -rf -- *.o *.lst *.d *.svg modbus2aprs
	rm -rf -- ./wjwwood_serial/src/*.d ./wjwwood_serial/src/*.o ./wjwwood_serial/src/impl/*.o ./wjwwood_serial/src/impl/*.d

le-01mr: $(OBJS)
	@echo 'linking $@'
	g++ -o $(@) $(OBJS) $(LIBS_DIRS) $(LIBS) 
	objdump --source --all-headers --demangle --line-numbers --wide "$@" > "$@.lst"

%.o : %.cc
	@echo 'Building: $<'
	g++ $(DEFINES) $(INCLUDES) -std=c++20 -Os $(WARNS) -c -fmessage-length=0 -MMD -MP -MF"$(patsubst %.o,%.d,$(@))" -MT"$(@)" -o "$(@)" "$(<)"

%.o : %.cpp
	@echo 'Building: $<'
	g++ $(DEFINES) $(INCLUDES) -std=c++20 -Os $(WARNS) -c -fmessage-length=0 -MMD -MP -MF"$(patsubst %.o,%.d,$(@))" -MT"$(@)" -o "$(@)" "$(<)"