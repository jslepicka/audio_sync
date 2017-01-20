/*
Copyright(c) 2017, James Slepicka
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met :

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and / or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <chrono>
#include <cstdint>
#include <mutex>
#include "resampler.hpp"

//#define SDL_MAIN_HANDLED
#pragma comment(lib, "sdl2.lib")
#pragma comment(lib, "sdl2main.lib")
#include "SDL.h"
#include "SDL_audio.h"

SDL_Renderer *renderer = NULL;
std::mutex buffer_mutex;

static const int SAMPLE_RATE = 48000;
static const float MIN_RATE = SAMPLE_RATE * .98f;
static const float MAX_RATE = SAMPLE_RATE * 1.02f;

static const int BUF_LEN = 4096;
struct s_buffer
{
	int write;
	int play;
	int16_t data[BUF_LEN] = { 0 };
} buffer;

void callback(void *udata, uint8_t *stream, int len)
{
	std::lock_guard<std::mutex> lock(buffer_mutex);
	s_buffer *b = (s_buffer*)udata;
	int first = len / 2;
	int second = 0;
	if (b->play + first >= BUF_LEN)
	{
		first = BUF_LEN - b->play;
		second = len / 2 - first;
	}
	memcpy(stream, (uint8_t*)(b->data + b->play), first * 2);
	if (second)
		memcpy(stream + first, b->data, second * 2);
	b->play = (b->play + len / 2) % BUF_LEN;
}


static const int NUM_W_VALUES = 60;
int w_values[NUM_W_VALUES];
int w_value_index = 0;

float rate_values[NUM_W_VALUES];

float calc_slope()
{
	int valid_values = 0;
	int sx = 0;
	int sy = 0;
	int sxx = 0;
	int sxy = 0;
	for (int i = w_value_index, j = 0; j < NUM_W_VALUES; j++)
	{
		if (w_values[i] != -1)
		{
			int k = valid_values;
			sx += k;
			sy += w_values[i];
			sxx += (k*k);
			sxy += (k*w_values[i]);
			valid_values++;
		}
		i = (i + 1) % NUM_W_VALUES;
	}
	float num = (float)(valid_values * sxy - sx*sy);
	float den = (float)(valid_values * sxx - sx*sx);
	return den == 0.0f ? 0.0f : num / den;
}

int get_writeable()
{
	std::lock_guard<std::mutex> lock(buffer_mutex);
	int pos;
	if (buffer.write <= buffer.play)
		pos = buffer.play - buffer.write;
	else
		pos = BUF_LEN - buffer.write + buffer.play;
	return pos*2;
}

void init_sound()
{
	buffer.play = 0;
	buffer.write = BUF_LEN / 2;

	SDL_AudioSpec wanted;
	wanted.freq = SAMPLE_RATE;
	wanted.format = AUDIO_S16;
	wanted.channels = 1;
	wanted.samples = 512;
	wanted.callback = callback;
	wanted.userdata = &buffer;

	SDL_OpenAudio(&wanted, NULL);
	SDL_PauseAudio(0);

	memset(w_values, -1, sizeof(w_values));
}

float tone[6400];

void draw()
{
	static int l = 0;
	//clear background
	SDL_SetRenderDrawColor(renderer, 64, 64, 64, 255);
	SDL_RenderClear(renderer);

	//draw buffer position line
	SDL_SetRenderDrawColor(renderer, 100, 255, 100, 255);
	int v = w_value_index;
	for (int i = 0; i < 60; i++)
	{
		SDL_SetRenderDrawColor(renderer, 64, 220, 64, 255);
		SDL_Rect rect;
		rect.x = i * 10;
		rect.w = 10;
		float h = w_values[v] / ((float)BUF_LEN * 2) * 480.0f;
		float r = (rate_values[v] - MIN_RATE) / (MAX_RATE - MIN_RATE) * 480.0f;
		v = (v + 1) % NUM_W_VALUES;
		rect.y = (int)(480.0 - h);
		rect.h = 2;
		SDL_RenderFillRect(renderer, &rect);

		SDL_SetRenderDrawColor(renderer, 220, 64, 64, 255);
		rect.y = (int)(480.0 - r);
		rect.h = 4;
		SDL_RenderFillRect(renderer, &rect);

	}
	SDL_RenderPresent(renderer);
}

int main(int argc, char *argv[])
{
	SDL_Window *window = SDL_CreateWindow
	(
		"audio_sync", SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		600,
		480,
		SDL_WINDOW_SHOWN
	);
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

	init_sound();

	//init resampler
	float output_rate = SAMPLE_RATE;
	float input_rate = output_rate * 8.0f; //input is 8x oversampled
	c_resampler resampler(input_rate, output_rate);

	//fill tone buffer
	float vol = .25;
	for (int i = 0; i < 6400; i++)
	{
		if (i % 800 == 0)
			vol *= -1.0;
		tone[i] = vol;
	}

	static const int ADJUST_FRAMES = 3;
	int adjust_period = ADJUST_FRAMES;
	float slope = 0.0;
	float prev_y = 0.0;
	float fps = 0.0f;
	int title_interval = 0;

	std::chrono::time_point<std::chrono::system_clock> start, end;
	start = std::chrono::system_clock::now();

	SDL_Event event;
	int quit = 0;
	while (!quit)
	{
		while (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
			case SDL_QUIT:
				quit = 1;
				break;
			}
		}

		draw();

		//generate audio
		for (int i = 0; i < 6400; i++)
		{
			resampler.process(tone[i]);
		}
		static const int16_t *resampled;
		int sample_count = resampler.get_output_buf(&resampled);
		
		//spin until there is enough room in buffer
		while (get_writeable() < sample_count*2);

		{
			std::lock_guard<std::mutex> lock(buffer_mutex);
			for (int i = 0; i < sample_count; i++)
			{
				buffer.data[buffer.write] = *resampled++;
				buffer.write = (buffer.write + 1) % BUF_LEN;
			}
		}

		end = std::chrono::system_clock::now();
		std::chrono::duration<float> elapsed = end - start;
		start = end;
		fps += elapsed.count();

		int w = get_writeable();

		//low-pass exponential moving average
		float a = .05f;
		float y = prev_y * (1.0f - a) + w * a;
		prev_y = y;
		w = (int)y;

		w_values[w_value_index] = w;
		rate_values[w_value_index] = output_rate;
		w_value_index = (w_value_index + 1) % NUM_W_VALUES;
		
		static const int TARGET = 5000;
		if (--adjust_period == 0)
		{
			slope = calc_slope();
			adjust_period = ADJUST_FRAMES;
			int diff = w - TARGET;

			//dir = 1 if diff > 0, -1 if diff < 0, else 0
			int dir = (diff > 0) - (diff < 0);

			float new_adj = 0.0;

			if (dir * slope < -1.0) //moving towards target at a slope > 1.0
			{
				new_adj = abs(slope) / 4.0f;
				if (new_adj > 1.0f)
					new_adj = 1.0f;
			}
			else if (dir * slope > 0.0f || w == 0) //moving away from target or stuck behind play cursor
			{
				//skew causes new_adj to increase faster when we're farther away from the target
				float skew = (abs(diff) / 1600.0f) * 10.0f;
				new_adj = -((abs(slope) + skew) / 2.0f);
				if (new_adj < -2.0f)
					new_adj = -2.0f;
			}
			new_adj *= dir;
			output_rate += -new_adj;
			if (output_rate < MIN_RATE)
				output_rate = MIN_RATE;
			else if (output_rate > MAX_RATE)
				output_rate = MAX_RATE;
			resampler.set_output_rate(output_rate);
		}

		if (++title_interval == 10)
		{
			title_interval = 0;
			char title[128];
			sprintf(title, "fps: %.2f, w: %d, freq: %.2f, slope: %.2f", 1.0f/(fps/10.0f), w, output_rate, slope);
			SDL_SetWindowTitle(window, title);
			fps = 0.0;
		}
	}
	return 0;
};