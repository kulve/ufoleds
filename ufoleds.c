/*
 * Copyright (c) 2013 Tuomas Kulve <tuomas@kulve.fi>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

/*
 * Some code cut'n'pasted from:
 * - alsaequal-0.6
 * - gstspectrascope.c in gst-plugins-bad
 */

#include <stdio.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <gst/fft/gstffts16.h>
#include <termios.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int open_serial_port(const char *port);

typedef struct snd_pcm_ufoleds {
  snd_pcm_extplug_t ext;
  GstFFTS16 *fft_ctx;
  GstFFTS16Complex *freq_data;
  int req_spf;
  guint num_freq;
  int fd;
  float max_sum;
} snd_pcm_ufoleds_t;

static snd_pcm_sframes_t ufoleds_transfer(snd_pcm_extplug_t *ext,
                                          const snd_pcm_channel_area_t *dst_areas,
                                          snd_pcm_uframes_t dst_offset,
                                          const snd_pcm_channel_area_t *src_areas,
                                          snd_pcm_uframes_t src_offset,
                                          snd_pcm_uframes_t size)
{
  snd_pcm_ufoleds_t *ufoleds = ext->private_data;
  gint16 *src, *dst;
  const guint ch = 2;
  static gint16 adata[16483];
  static int adata_i = 0;
  GstFFTS16Complex *fdata = ufoleds->freq_data;

  guint x;
  gfloat fr, fi;
  gfloat sum = 0;

  //SNDERR("size: %d", size);

  src = (gint16*)(src_areas->addr +
                  (src_areas->first + src_areas->step * src_offset)/8);
  dst = (gint16*)(dst_areas->addr +
                  (dst_areas->first + dst_areas->step * dst_offset)/8);

  /* Copy data onwards */
  memcpy(dst, src, size * sizeof(gint16) * ch);

  /* Deinterleave and mixdown adata */
  if (ch == 2) {
    guint i, c, v, s = 0;

    for (i = 0; i < size; i++) {
      v = 0;
      for (c = 0; c < ch; c++) {
        v += src[s++];
      }

      /* Ignore extra data in case of buffer overflow */
      if (adata_i >= sizeof(adata)) {
        break;
      }
      adata[adata_i++] = v / ch;
    }
  }

  if (adata_i < ufoleds->req_spf) {
    return size;
  }

  /* Run fft */
  gst_fft_s16_window(ufoleds->fft_ctx, adata, GST_FFT_WINDOW_HAMMING);
  gst_fft_s16_fft(ufoleds->fft_ctx, adata, fdata);

  /* Remove processed data */
  memcpy(adata, &adata[ufoleds->req_spf], sizeof(gint16) * (adata_i - ufoleds->req_spf));
  adata_i = 0;

  /* Calculate the "power" of the audio samples, ignore higher hal */
  for (x = 0; x < (ufoleds->num_freq - 1)/2; x++) {
    gfloat booster;
    fr = (gfloat)fdata[1 + x].r / 512.0;
    fi = (gfloat)fdata[1 + x].i / 512.0;
    booster = ufoleds->num_freq - x;
    sum += fabs(fr * fr + fi * fi) * booster;
  }

  /* Slowly decrease the seen maximum */
  ufoleds->max_sum -= 0.01;

  /* Check for new maximum */
  if (sum > ufoleds->max_sum) {
    ufoleds->max_sum = sum;
  }

  /* Scale to 0-10 for 10 steps */
  sum /= ufoleds->max_sum;
  sum *= 7;

  //SNDERR("sum: %f", sum);

  if (ufoleds->fd > -1) {
    char buf[8] = "G0000\r\n\0";
    int x;

    for(x = 1; x < 5; ++x) {
      if (x < sum) {
        buf[x] = '1';
      }
    }
    write(ufoleds->fd, buf, sizeof(buf) - 1);
    //SNDERR("%s", buf);
  } else {
    //SNDERR("NO FD OPEN");
  }

  return size;
}

static int ufoleds_close(snd_pcm_extplug_t *ext) {
  snd_pcm_ufoleds_t *ufoleds = ext->private_data;

  SNDERR("Closing");

  if (ufoleds->fd > -1) {
    char buf[8] = "G0000\r\n";
    write(ufoleds->fd, buf, sizeof(buf));
  }

  if (ufoleds->fft_ctx) {
    gst_fft_s16_free (ufoleds->fft_ctx);
    ufoleds->fft_ctx = NULL;
  }

  if (ufoleds->freq_data) {
    g_free (ufoleds->freq_data);
    ufoleds->freq_data = NULL;
  }

  if (ufoleds->fd > -1) {
    close(ufoleds->fd);
    ufoleds->fd = -1;
  }

  free(ufoleds);
  return 0;
}

static int ufoleds_init(snd_pcm_extplug_t *ext)
{
  snd_pcm_ufoleds_t *ufoleds = ext->private_data;

  SNDERR("Initialisating");

  /* Counted so that ufoleds->req_spf is ~1024 and matches gst_fft_next_fast_length() */
  ufoleds->num_freq = 541;

  /* we'd need this amount of samples per render() call */
  ufoleds->req_spf = ufoleds->num_freq * 2 - 2; // 1080
  ufoleds->fft_ctx = gst_fft_s16_new(ufoleds->req_spf, FALSE);
  ufoleds->freq_data = g_new(GstFFTS16Complex, ufoleds->num_freq);

  ufoleds->max_sum = 0;

  if (ufoleds->fd == -1) {
    int i;
    ufoleds->fd = open_serial_port("/dev/ttyUSB0");
    if (ufoleds->fd == -1) {
      SNDERR("open_serial_port() failed");
      return 0;
    }

    for (i = 1; i < 5; i++) {
      char buf[8] = "G0000\r\n";
      if (i > 1)
        buf[i - 1] = '0';
      buf[i] = '1';
      write(ufoleds->fd, buf, sizeof(buf));
      usleep(200*1000);
    }

    {
      char buf[8] = "G0000\r\n";
      write(ufoleds->fd, buf, sizeof(buf));
    }
  }

  return 0;
}

static snd_pcm_extplug_callback_t ufoleds_callback = {
  .transfer = ufoleds_transfer,
  .init = ufoleds_init,
  .close = ufoleds_close,
};


/*
 * Open a serial device
 */
int open_serial_port(const char *port)
{
  int serialFD;
  struct termios newtio;

  // Open device
  serialFD = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (serialFD < 0) {
    SNDERR("Failed to open Control Board device %s: %s", port, strerror(errno));
    return -1;
  }

  // clear struct for new port settings
  bzero(&newtio, sizeof(newtio));

  // control mode flags
  newtio.c_cflag = CS8 | CLOCAL | CREAD;

  // input mode flags
  newtio.c_iflag = 0;

  // output mode flags
  newtio.c_oflag = 0;

  // local mode flags
  newtio.c_lflag = 0;

  // set input/output speeds
  cfsetispeed(&newtio, B115200);
  cfsetospeed(&newtio, B115200);

  // now clean the serial line and activate the settings
  tcflush(serialFD, TCIFLUSH);
  tcsetattr(serialFD, TCSANOW, &newtio);

  return serialFD;
}

SND_PCM_PLUGIN_DEFINE_FUNC(ufoleds)
{
  snd_config_iterator_t i, next;
  snd_pcm_ufoleds_t *ufoleds;
  snd_config_t *sconf = NULL;
  int err;
	
  /* Parse configuration options from asoundrc */
  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if (snd_config_get_id(n, &id) < 0)
      continue;
    if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
      continue;
    if (strcmp(id, "slave") == 0) {
      sconf = n;
      continue;
    }

    SNDERR("Unknown field %s", id);
    return -EINVAL;
  }

  /* Make sure we have a slave and control devices defined */
  if (!sconf) {
    SNDERR("No slave configuration for UFO LEDs");
    return -EINVAL;
  }

  /* Intialize the local object data */
  ufoleds = calloc(1, sizeof(*ufoleds));
  if (ufoleds == NULL)
    return -ENOMEM;

  ufoleds->fd = -1;
  ufoleds->ext.version = SND_PCM_EXTPLUG_VERSION;
  ufoleds->ext.name = "UFO LEDs Plugin";
  ufoleds->ext.callback = &ufoleds_callback;
  ufoleds->ext.private_data = ufoleds;

  /* Create the ALSA External Plugin */
  err = snd_pcm_extplug_create(&ufoleds->ext, name, root, sconf, stream, mode);
  if (err < 0) {
    SNDERR("snd_pcm_extplug_create() failed");
    return err;
  }

  /* Set PCM Contraints */
  snd_pcm_extplug_set_param(&ufoleds->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2);
  snd_pcm_extplug_set_slave_param(&ufoleds->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2);

  snd_pcm_extplug_set_param(&ufoleds->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);
  snd_pcm_extplug_set_slave_param(&ufoleds->ext,SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);

  *pcmp = ufoleds->ext.pcm;
	
  return 0;

}

SND_PCM_PLUGIN_SYMBOL(ufoleds);


/* Emacs indentatation information
   Local Variables:
   indent-tabs-mode:nil
   tab-width:2
   c-basic-offset:2
   End:
*/
