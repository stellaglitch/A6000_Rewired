# The desktop build of the a6000 pipeline: the same C the camera runs.
#   make            -> rewire (rewire.exe on Windows)
#   make test       -> runs the codec self-test and a bend over samples/

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
SRC     = rewire.c ../native/arw.c ../native/recipe.c ../native/jpegenc.c ../native/pipeline.c ../native/log.c ../native/mexp.c \
          ../native/engine/bend.c ../native/engine/bendx.c
SAMPLE  = ../samples/DSC01542.ARW
OUT     = ../out/host

rewire: $(SRC) ../native/*.h ../native/engine/*.h
	$(CC) $(CFLAGS) -I../native -I../native/engine -o $@ $(SRC) -lm

test: rewire
	mkdir -p $(OUT)
	./rewire --info $(SAMPLE)
	./rewire --selftest $(SAMPLE)
	./rewire $(SAMPLE) -o $(OUT)/plain.ARW -j $(OUT)/plain.JPG
	./rewire $(SAMPLE) -r 7 -o $(OUT)/random7.ARW -j $(OUT)/random7.JPG -b $(OUT)/random7.BND
	./rewire $(SAMPLE) -x Melt:0:8 -j $(OUT)/melt.JPG
	# The streamed path (a 256-row ring) must give exactly what the whole frame
	# gives, buses and all, for an unsegmented matrix.
	./rewire $(SAMPLE) -r 21 -B -d 6 -o $(OUT)/ring.ARW -j $(OUT)/ring.JPG -l $(OUT)/test-log.txt
	./rewire $(SAMPLE) -r 21 -B -d 6 -o $(OUT)/frame.ARW -j $(OUT)/frame.JPG -F -l $(OUT)/test-log.txt
	cmp $(OUT)/ring.ARW $(OUT)/frame.ARW
	cmp $(OUT)/ring.JPG $(OUT)/frame.JPG
	@echo "streamed == whole frame"
	# Multiple exposure. A picture averaged or lightened with itself is itself.
	./rewire --mexp-selftest
	./rewire $(SAMPLE) -e $(SAMPLE) -M average -o $(OUT)/mexp-self.ARW -j $(OUT)/mexp-self.JPG -l $(OUT)/test-log.txt
	cmp $(OUT)/plain.ARW $(OUT)/mexp-self.ARW
	cmp $(OUT)/plain.JPG $(OUT)/mexp-self.JPG
	./rewire $(SAMPLE) -e $(SAMPLE) -M lighten -o $(OUT)/mexp-self.ARW
	cmp $(OUT)/plain.ARW $(OUT)/mexp-self.ARW
	@echo "average and lighten of a shot with itself == the shot"
	# Blended before the bend and after it, streamed and whole-frame alike.
	./rewire $(SAMPLE) -e $(OUT)/random7.ARW -M darken -r 21 -B -d 6 -o $(OUT)/mexp-ring.ARW -j $(OUT)/mexp-ring.JPG
	./rewire $(SAMPLE) -e $(OUT)/random7.ARW -M darken -r 21 -B -d 6 -o $(OUT)/mexp-frame.ARW -j $(OUT)/mexp-frame.JPG -F
	cmp $(OUT)/mexp-ring.ARW $(OUT)/mexp-frame.ARW
	./rewire $(SAMPLE) -e $(OUT)/random7.ARW -M add -A -r 21 -B -d 6 -o $(OUT)/mexp-ring.ARW -j $(OUT)/mexp-post.JPG
	./rewire $(SAMPLE) -e $(OUT)/random7.ARW -M add -A -r 21 -B -d 6 -o $(OUT)/mexp-frame.ARW -F
	cmp $(OUT)/mexp-ring.ARW $(OUT)/mexp-frame.ARW
	./rewire $(SAMPLE) -e $(OUT)/random7.ARW -e $(OUT)/plain.ARW -M lighten -x Melt:0:8 -j $(OUT)/mexp-three.JPG
	@echo "multiple exposure: streamed == whole frame, before and after the bend"

clean:
	rm -f rewire rewire.exe
