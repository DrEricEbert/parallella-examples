#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include <jpeglib.h>

/* Convert to grayscale float
 *
 * Assumes 8-bit grayscale input data
 *
 * @return TRUE on success. FALSE on failure.
 */
static inline bool to_float(struct jpeg_decompress_struct *cinfo,
			    uint8_t **bufs, size_t nbufs, float *bitmap,
			    size_t line)
{
	const float factor = 1.0f/255.0f;
	float *out;
	size_t i, j;

	/* Only support 8-bit grayscale for now */
	if (cinfo->data_precision != 8 || cinfo->output_components != 1) {
		fprintf(stderr, "%s:%s: Unsupported color format\n",
				__FILE__, __func__);
		return false;
	}

	/* Advance bitmap */
	out = bitmap + line * cinfo->output_width;

	for (i = 0; i < nbufs; i++) {
		for (j = 0; j < cinfo->output_width; j++)
			*out++ = ((float) bufs[i][j]) * factor;
	}
	return true;
}

/* Convert from grayscale float
 *
 * Assumes 8-bit grayscale output data
 *
 * @return TRUE on success. FALSE on failure.
 */
static inline bool from_float(struct jpeg_compress_struct *cinfo,
			      uint8_t **bufs, size_t nbufs, float *bitmap,
			      size_t line)
{
	const float factor = 255.0f;
	float *in;
	size_t i, j;

	/* Only support 8-bit grayscale for now */
	if (cinfo->data_precision != 8 || cinfo->input_components != 1) {
		fprintf(stderr, "%s:%s: Unsupported color format\n",
				__FILE__, __func__);
		return false;
	}

	/* Advance bitmap */
	in = bitmap + line * cinfo->image_width;

	for (i = 0; i < nbufs; i++) {
		for (j = 0; j < cinfo->image_width; j++)
			bufs[i][j] = (uint8_t) (*in++ * factor);
	}
	return true;
}

/* Decompress jpeg to raw float grayscale
 *
 * @return pointer to data. Caller need to free data.
 */
float *jpeg_to_grayscale(void *jpeg, size_t jpeg_size, int *width, int *height)
{
	int ret, line = 0;
	float *bitmap;
	uint8_t *buf, *bufs[1];
	bool convert_fail = false;

	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&cinfo);

	jpeg_mem_src(&cinfo, jpeg, jpeg_size);

	ret = jpeg_read_header(&cinfo, TRUE);
	if (ret != JPEG_HEADER_OK) {
		fprintf(stderr, "%s:%s: Not a JPEG\n", __FILE__, __func__);
		return NULL;
	}

	cinfo.out_color_space = JCS_GRAYSCALE;

	jpeg_start_decompress(&cinfo);

	*width = cinfo.output_width;
	*height = cinfo.output_height;

	bitmap = calloc((*width) * (*height), sizeof(float));

	buf = calloc(*width, sizeof(uint8_t));
	bufs[0] = buf;

	while (cinfo.output_scanline < cinfo.output_height) {
		jpeg_read_scanlines(&cinfo, bufs, 1);
		if (!to_float(&cinfo, bufs, 1, bitmap, line)) {
			convert_fail = true;
			break;
		}
		line++;
	}

	jpeg_finish_decompress(&cinfo);

	jpeg_destroy_decompress(&cinfo);

	free(buf);

	if (convert_fail) {
		free(bitmap);
		bitmap = NULL;
	}

	return bitmap;
}

/* In-memory conversion from float grayscale bitmap to grayscale JPEG
 *
 * @return pointer to JPEG data. Caller need to free data.
 */
void *grayscale_to_jpeg(float *bitmap, int width, int height,
			unsigned long *jpeg_size)
{
	unsigned char *stride;
	uint8_t *buf, *bufs[1];
	uint8_t *jpeg;
	int line;
	bool convert_fail = false;

	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;


	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);

	jpeg_mem_dest(&cinfo, &jpeg, jpeg_size);

	cinfo.image_width = width;
	cinfo.image_height = height;
	cinfo.input_components = 1;
	cinfo.in_color_space = JCS_GRAYSCALE;

	jpeg_set_defaults(&cinfo);

	jpeg_start_compress(&cinfo, TRUE);

	buf = calloc(width, sizeof(uint8_t));
	for (line = 0; line < height; line++) {
		bufs[0] = buf;
		if (!from_float(&cinfo, bufs, 1, bitmap, line)) {
			convert_fail = true;
			break;
		}
		jpeg_write_scanlines(&cinfo, bufs, 1);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);

	free(buf);

	if (convert_fail) {
		free(jpeg);
		jpeg = NULL;
	}

	return jpeg;
}

/* Convert float bitmap to JPEG file
 *
 * @return True on success, False on failure
 */
bool grayscale_to_jpeg_file(float *bitmap, int width, int height, char *path)
{
	int fd;
	struct stat file_stat;
	size_t i;
	ssize_t count;
	uint8_t *jpeg = NULL;
	unsigned long jpeg_size = 0;
	bool ret = true;

	jpeg = grayscale_to_jpeg(bitmap, width, height, &jpeg_size);
	if (!jpeg)
		return false;

	fd = open(path, O_WRONLY| O_CREAT, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s: open: %s\n", path, strerror(errno));
		ret = false;
		goto out;
	}

	for (i = 0, count = 0; i < jpeg_size; i += count) {
		count = write(fd, &jpeg[i], jpeg_size - i);
		if (count < 0) {
			fprintf(stderr, "%s: write: %s\n",
				path, strerror(errno));
			ret = false;
			goto out;
		}
	}

out:
	free(jpeg);
	close(fd);
	return ret;
}

/* Decompress JPEG file to float bitmap
 *
 * @return pointer to data. Caller need to free data.
 */
float *jpeg_file_to_grayscale(char *path, int *width, int *height)
{
	int ret, fd;
	uint8_t *jpeg;
	size_t jpeg_size;
	struct stat file_stat;
	size_t i;
	ssize_t count;
	float *bitmap = NULL;

	ret = stat(path, &file_stat);
	if (ret) {
		fprintf(stderr, "%s: stat: %s\n", path, strerror(errno));
		return NULL;
	}

	jpeg_size = file_stat.st_size;
	jpeg = malloc(jpeg_size);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: open: %s\n", path, strerror(errno));
		goto out;
	}

	for (i = 0, count = 0; i < jpeg_size; i += count) {
		count = read(fd, &jpeg[i], jpeg_size - i);
		if (count < 0) {
			fprintf(stderr, "%s: read: %s\n",
				path, strerror(errno));
			goto out;
		}
	}

	bitmap = jpeg_to_grayscale(jpeg, jpeg_size, width, height);

out:
	free(jpeg);
	close(fd);
	return bitmap;
}

