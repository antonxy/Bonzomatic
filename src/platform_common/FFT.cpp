#include <stdio.h>
#include "../FFT.h"




#include <stdio.h>
#include <complex.h>
#include <tgmath.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>

#include <pulse/simple.h>
#include <pulse/error.h>

#include <fftw3.h>

#include <deque>

namespace FFT
{
/*
 * pa_fft is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * pa_fft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with pa_fft.  If not, see <http://www.gnu.org/licenses/>.
 */

enum w_type {
    WINDOW_TRIANGLE,
    WINDOW_HANNING,
    WINDOW_HAMMING,
    WINDOW_BLACKMAN,
    WINDOW_BLACKMAN_HARRIS,
    WINDOW_WELCH,
    WINDOW_FLAT,
};

struct pa_fft {
    pthread_t thread;
    bool stop, log_graph, overlap, no_refresh;
    int cont;

    /* Pulse */
    pa_simple *s;
    const char *dev;
    int error;
    pa_sample_spec ss;
    pa_channel_map map;

    /* Buffer */
    float *pa_buf;
    size_t pa_buf_size;
    unsigned int pa_samples;
    double *buffer;
    size_t buffer_size;
    unsigned int buffer_samples;
    float **frame_avg_mag;
    unsigned int size_avg;
    unsigned int frame_avg;

    /* FFT */
    int fft_flags;
    unsigned int start_low;
    enum w_type win_type;
    fftw_complex *output;
    unsigned int output_size;
    unsigned int fft_memb;
    double fft_fund_freq;
    fftw_plan plan;

		/* Beat detection */
		std::deque<double> energies;
		double beat;
};

void deinit_fft(struct pa_fft *pa_fft) {
    if (!pa_fft)
        return;

    if (pa_fft->cont != 2) {
        pa_fft->cont = 0;
        sleep(1);
    }

    fftw_destroy_plan(pa_fft->plan);
    fftw_free(pa_fft->output);

    if (pa_fft->cont != 2)
        pa_simple_free(pa_fft->s);
    free(pa_fft->buffer);
    free(pa_fft->pa_buf);

    pthread_join(pa_fft->thread, NULL);

    free(pa_fft);
}

static inline void avg_buf_init(struct pa_fft *pa_fft)
{
    pa_fft->frame_avg_mag = (float**) malloc(pa_fft->fft_memb*sizeof(float *));
    for (int i = 0; i < pa_fft->fft_memb; i++)
        pa_fft->frame_avg_mag[i] = (float*)calloc(pa_fft->frame_avg, sizeof(float));
}

static inline void weights_init(float *dest, int samples, enum w_type w)
{
    switch(w) {
        case WINDOW_TRIANGLE:
            for (int i = 0; i < samples; i++)
                dest[i] = 1 - 2*fabsf((i - ((samples - 1)/2.0f))/(samples - 1));
            break;
        case WINDOW_HANNING:
            for (int i = 0; i < samples; i++)
                dest[i] = 0.5f*(1 - cos((2*M_PI*i)/(samples - 1)));
            break;
        case WINDOW_HAMMING:
            for (int i = 0; i < samples; i++)
                dest[i] = 0.54 - 0.46*cos((2*M_PI*i)/(samples - 1));
            break;
        case WINDOW_BLACKMAN:
            for (int i = 0; i < samples; i++) {
                const float c1 = cos((2*M_PI*i)/(samples - 1));
                const float c2 = cos((4*M_PI*i)/(samples - 1));
                dest[i] = 0.42659 - 0.49656*c1 + 0.076849*c2;
            }
            break;
        case WINDOW_BLACKMAN_HARRIS:
            for (int i = 0; i < samples; i++) {
                const float c1 = cos((2*M_PI*i)/(samples - 1));
                const float c2 = cos((4*M_PI*i)/(samples - 1));
                const float c3 = cos((6*M_PI*i)/(samples - 1));
                dest[i] = 0.35875 - 0.48829*c1 + 0.14128*c2 - 0.001168*c3;
            }
            break;
        case WINDOW_FLAT:
            for (int i = 0; i < samples; i++)
                dest[i] = 1.0f;
            break;
        case WINDOW_WELCH:
            for (int i = 0; i < samples; i++)
                dest[i] = 1 - pow((i - ((samples - 1)/2.0f))/((samples - 1)/2.0f), 2.0f);
            break;
        default:
            for (int i = 0; i < samples; i++)
                dest[i] = 0.0f;
            break;
    }
    float sum = 0.0f;
    for (int i = 0; i < samples; i++)
        sum += dest[i];
    for (int i = 0; i < samples; i++)
        dest[i] /= sum;
}

static inline void apply_win(double *dest, float *src, float *weights,
                             int samples)
{
    for (int i = 0; i < samples; i++)
        dest[i] = src[i]*weights[i];
}

static inline float frame_average(float mag, float *buf, int avgs, int no_mod)
{
    if (!avgs)
        return mag;
    float val = mag;
    for (int i = 0; i < avgs; i++)
        val += buf[i];
    val /= avgs + 1;
    if (no_mod)
        return val;
    for (int i = avgs - 1; i > 0; i--)
        buf[i] = buf[i-1];
    buf[0] = mag;
    return val;
}

void *pa_fft_thread(void *arg) {
    struct pa_fft *t = (struct pa_fft *)arg;
    float weights[t->buffer_samples];

    avg_buf_init(t);
    weights_init(weights, t->fft_memb, t->win_type);

    while (t->cont) {

        if (t->overlap)
            memcpy(&t->pa_buf[0], &t->pa_buf[t->pa_samples],
                   t->pa_samples*sizeof(float));

        pa_usec_t lag = pa_simple_get_latency(t->s, &t->error);

        if (pa_simple_read(t->s, &t->pa_buf[t->overlap ? t->pa_samples : 0],
            t->pa_buf_size, &t->error) < 0) {
            fprintf(stderr, __FILE__": pa_simple_read() failed: %s\n",
                    pa_strerror(t->error));
            t->cont = 0;
            continue;
        }

				double energy = 0;
				for (size_t i = 0; i < t->buffer_samples; ++i) {
					double val = t->pa_buf[i];
					energy += val * val;
				}
				t->energies.push_back(energy);
				while (t->energies.size() > 40) {
					t->energies.pop_front();
				}

				//double avg_energy = 0;
				//for (double en : t->energies) {
				//	avg_energy += en;
				//}
				//avg_energy /= t->energies.size();

				//double energy_var = 0;
				//for (double en : t->energies) {
				//	float v = avg_energy - en;
				//	energy_var += v * v;
				//}
				//energy_var /= t->energies.size();


        apply_win(t->buffer, t->pa_buf, weights, t->buffer_samples);
        fftw_execute(t->plan);


				double avg_energy = 0;
				int lower_cut = 10;
				int upper_cut = 1;
				int start_low = t->fft_memb - (t->fft_memb - t->start_low) / lower_cut;
				int end_high = t->fft_memb / upper_cut;
				for (int i = start_low; i < end_high; i++) {
					std::complex<double> num = *(reinterpret_cast<std::complex<double>*>(&t->output[i]));
					double mag = std::real(num)*std::real(num) + std::imag(num)*std::imag(num);
					avg_energy += mag;
				}
				avg_energy /= end_high  - start_low;

				double energy_var = 0;
				for (int i = start_low; i < end_high; i++) {
					std::complex<double> num = *(reinterpret_cast<std::complex<double>*>(&t->output[i]));
					double mag = std::real(num)*std::real(num) + std::imag(num)*std::imag(num);
					double v = avg_energy - mag;
					energy_var += v * v;
				}
				energy_var /= end_high - start_low;

				double c = -0.0000015*energy_var+1.5142857;
				t->beat = energy - c * avg_energy;


        if ((float)lag/1000000 >= 1.0f)
            fprintf(stderr, "Audio lagging\n");
        for (int i = t->start_low; i < t->fft_memb; i++) {
            std::complex<double> num = *(reinterpret_cast<std::complex<double>*>(&t->output[i]));
            double mag = std::real(num)*std::real(num) + std::imag(num)*std::imag(num);
            mag = log10(mag)/10;
            mag = frame_average(mag, t->frame_avg_mag[i], t->frame_avg, 0);
        }

    }

    deinit_fft(t);

    return NULL;
}

static inline void init_pulse(struct pa_fft *pa_fft)
{
    /* PA spec */
    fprintf(stderr, "device = %s\n", pa_fft->dev);
    if (!pa_fft->dev) {
        fprintf(stderr, "Warning: no device specified! It's highly possible "
                        "Pulseaudio will attempt to use the microphone!\n");
    }

    pa_fft->ss.format = PA_SAMPLE_FLOAT32LE;
    pa_fft->ss.rate = 44100;
    pa_fft->ss.channels = 1;
    pa_channel_map_init_mono(&pa_fft->map);

    if (!(pa_fft->s = pa_simple_new(NULL, "pa_fft", PA_STREAM_RECORD, pa_fft->dev,
                                    "record", &pa_fft->ss, &pa_fft->map, NULL,
                                    &pa_fft->error))) {
        fprintf(stderr, __FILE__": pa_simple_new() failed: %s\n",
                pa_strerror(pa_fft->error));
        pa_fft->cont = 0;
        return;
    }
}

static inline void init_buffers(struct pa_fft *pa_fft)
{
    if (!pa_fft)
        return;

    /* Pulse buffer */
    pa_fft->pa_samples = pa_fft->buffer_samples/(pa_fft->overlap ? 2 : 1);
    pa_fft->pa_buf_size = sizeof(float)*pa_fft->buffer_samples;
    pa_fft->pa_buf = (float*)malloc(pa_fft->pa_buf_size);

    /* Input buffer */
    pa_fft->buffer_size = sizeof(double)*pa_fft->buffer_samples;
    pa_fft->buffer = (double*)malloc(pa_fft->buffer_size);

    /* FFTW buffer */
    pa_fft->output_size = sizeof(fftw_complex)*pa_fft->buffer_samples;
    pa_fft->output = (double(*)[2])fftw_malloc(pa_fft->output_size);
    pa_fft->fft_memb = (pa_fft->buffer_samples/2)+1;
    pa_fft->fft_fund_freq = (double)pa_fft->ss.rate/pa_fft->buffer_samples;
}

static inline void init_fft(struct pa_fft *pa_fft) {
    if (!pa_fft)
        return;

    pa_fft->plan = fftw_plan_dft_r2c_1d(pa_fft->buffer_samples, pa_fft->buffer,
                                        pa_fft->output, pa_fft->fft_flags);
}


  pa_fft * ctx = NULL;
  bool Open()
  {
    ctx = new pa_fft;
    ctx->cont = 1;

    ctx->buffer_samples = FFT_SIZE*2;
    ctx->dev = NULL;
    ctx->log_graph = 1;
    ctx->no_refresh = 0;
    ctx->overlap = 0;
    ctx->frame_avg = 2;
    ctx->start_low = 1;
    ctx->win_type = WINDOW_BLACKMAN_HARRIS;
    ctx->fft_flags = FFTW_PATIENT | FFTW_DESTROY_INPUT;
    ctx->dev = "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor";

    init_pulse(ctx);
    init_buffers(ctx);
    init_fft(ctx);

    if (!ctx->cont) {
        ctx->cont = 2;
        deinit_fft(ctx);
        return false;
    }

    pthread_create(&ctx->thread, NULL, pa_fft_thread, ctx);

    return true;
  }
  bool GetFFT( float * samples )
  {
    if (!ctx->cont)
      return false;
    for (int i = ctx->start_low; i < ctx->fft_memb; i++) {
        std::complex<double> num = *(reinterpret_cast<std::complex<double>*>(&ctx->output[i]));
        double mag = std::real(num)*std::real(num) + std::imag(num)*std::imag(num);
        mag = log10(mag)/10;
        samples[i] = mag;
    }

    return true;
  }
	float GetBeat() {
    if (!ctx->cont)
      return 0.0;
		return ctx->beat;
	}
  void Close()
  {
    ctx->cont = 0;
		delete ctx;
  }
}
