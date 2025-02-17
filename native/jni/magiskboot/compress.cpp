#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <memory>
#include <functional>

#include <zlib.h>
#include <bzlib.h>
#include <lzma.h>
#include <lz4.h>
#include <lz4frame.h>
#include <lz4hc.h>

#include <logging.h>
#include <utils.h>

#include "magiskboot.h"
#include "compress.h"

using namespace std;

#define bwrite filter_stream::write
#define bclose filter_stream::close

constexpr size_t CHUNK = 0x40000;
constexpr size_t LZ4_UNCOMPRESSED = 0x800000;
constexpr size_t LZ4_COMPRESSED = LZ4_COMPRESSBOUND(LZ4_UNCOMPRESSED);

class cpr_stream : public filter_stream {
public:
	explicit cpr_stream(FILE *fp) : filter_stream(fp) {}

	int read(void *buf, size_t len) final {
		return stream::read(buf, len);
	}

	int close() final {
		finish();
		return bclose();
	}

protected:
	// If finish is overridden, destroy should be called in the destructor
	virtual void finish() {}
	void destroy() { if (fp) finish(); }
};

class gz_strm : public cpr_stream {
public:
	~gz_strm() override { destroy(); }

	int write(const void *buf, size_t len) override {
		return len ? write(buf, len, Z_NO_FLUSH) : 0;
	}

protected:
	enum mode_t {
		DECODE,
		ENCODE
	} mode;

	gz_strm(mode_t mode, FILE *fp) : cpr_stream(fp), mode(mode) {
		switch(mode) {
			case DECODE:
				inflateInit2(&strm, 15 | 16);
				break;
			case ENCODE:
				deflateInit2(&strm, 9, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY);
				break;
		}
	}

	void finish() override {
		write(nullptr, 0, Z_FINISH);
		switch(mode) {
			case DECODE:
				inflateEnd(&strm);
				break;
			case ENCODE:
				deflateEnd(&strm);
				break;
		}
	}

private:
	z_stream strm;
	uint8_t outbuf[CHUNK];

	int write(const void *buf, size_t len, int flush) {
		strm.next_in = (Bytef *) buf;
		strm.avail_in = len;
		do {
			int code;
			strm.next_out = outbuf;
			strm.avail_out = sizeof(outbuf);
			switch(mode) {
				case DECODE:
					code = inflate(&strm, flush);
					break;
				case ENCODE:
					code = deflate(&strm, flush);
					break;
			}
			if (code == Z_STREAM_ERROR) {
				LOGW("gzip %s failed (%d)\n", mode ? "encode" : "decode", code);
				return -1;
			}
			bwrite(outbuf, sizeof(outbuf) - strm.avail_out);
		} while (strm.avail_out == 0);
		return len;
	}
};

class gz_decoder : public gz_strm {
public:
	explicit gz_decoder(FILE *fp) : gz_strm(DECODE, fp) {};
};

class gz_encoder : public gz_strm {
public:
	explicit gz_encoder(FILE *fp) : gz_strm(ENCODE, fp) {};
};

class bz_strm : public cpr_stream {
public:
	~bz_strm() override { destroy(); }

	int write(const void *buf, size_t len) override {
		return len ? write(buf, len, BZ_RUN) : 0;
	}

protected:
	enum mode_t {
		DECODE,
		ENCODE
	} mode;

	bz_strm(mode_t mode, FILE *fp) : cpr_stream(fp), mode(mode) {
		switch(mode) {
			case DECODE:
				BZ2_bzDecompressInit(&strm, 0, 0);
				break;
			case ENCODE:
				BZ2_bzCompressInit(&strm, 9, 0, 0);
				break;
		}
	}

	void finish() override {
		switch(mode) {
			case DECODE:
				BZ2_bzDecompressEnd(&strm);
				break;
			case ENCODE:
				write(nullptr, 0, BZ_FINISH);
				BZ2_bzCompressEnd(&strm);
				break;
		}
	}

private:
	bz_stream strm;
	char outbuf[CHUNK];

	int write(const void *buf, size_t len, int flush) {
		strm.next_in = (char *) buf;
		strm.avail_in = len;
		do {
			int code;
			strm.avail_out = sizeof(outbuf);
			strm.next_out = outbuf;
			switch(mode) {
				case DECODE:
					code = BZ2_bzDecompress(&strm);
					break;
				case ENCODE:
					code = BZ2_bzCompress(&strm, flush);
					break;
			}
			if (code < 0) {
				LOGW("bzip2 %s failed (%d)\n", mode ? "encode" : "decode", code);
				return -1;
			}
			bwrite(outbuf, sizeof(outbuf) - strm.avail_out);
		} while (strm.avail_out == 0);
		return len;
	}
};

class bz_decoder : public bz_strm {
public:
	explicit bz_decoder(FILE *fp) : bz_strm(DECODE, fp) {};
};

class bz_encoder : public bz_strm {
public:
	explicit bz_encoder(FILE *fp) : bz_strm(ENCODE, fp) {};
};

class lzma_strm : public cpr_stream {
public:
	~lzma_strm() override { destroy(); }

	int write(const void *buf, size_t len) override {
		return len ? write(buf, len, LZMA_RUN) : 0;
	}

protected:
	enum mode_t {
		DECODE,
		ENCODE_XZ,
		ENCODE_LZMA
	} mode;

	lzma_strm(mode_t mode, FILE *fp) : cpr_stream(fp), mode(mode), strm(LZMA_STREAM_INIT) {
		lzma_options_lzma opt;

		// Initialize preset
		lzma_lzma_preset(&opt, 9);
		lzma_filter filters[] = {
			{ .id = LZMA_FILTER_LZMA2, .options = &opt },
			{ .id = LZMA_VLI_UNKNOWN, .options = nullptr },
		};

		lzma_ret ret;
		switch(mode) {
			case DECODE:
				ret = lzma_auto_decoder(&strm, UINT64_MAX, 0);
				break;
			case ENCODE_XZ:
				ret = lzma_stream_encoder(&strm, filters, LZMA_CHECK_CRC32);
				break;
			case ENCODE_LZMA:
				ret = lzma_alone_encoder(&strm, &opt);
				break;
		}
	}

	void finish() override {
		write(nullptr, 0, LZMA_FINISH);
		lzma_end(&strm);
	}

private:
	lzma_stream strm;
	uint8_t outbuf[CHUNK];

	int write(const void *buf, size_t len, lzma_action flush) {
		strm.next_in = (uint8_t *) buf;
		strm.avail_in = len;
		do {
			strm.avail_out = sizeof(outbuf);
			strm.next_out = outbuf;
			int code = lzma_code(&strm, flush);
			if (code != LZMA_OK && code != LZMA_STREAM_END) {
				LOGW("LZMA %s failed (%d)\n", mode ? "encode" : "decode", code);
				return -1;
			}
			bwrite(outbuf, sizeof(outbuf) - strm.avail_out);
		} while (strm.avail_out == 0);
		return len;
	}
};

class lzma_decoder : public lzma_strm {
public:
	lzma_decoder(FILE *fp) : lzma_strm(DECODE, fp) {}
};

class xz_encoder : public lzma_strm {
public:
	xz_encoder(FILE *fp) : lzma_strm(ENCODE_XZ, fp) {}
};

class lzma_encoder : public lzma_strm {
public:
	lzma_encoder(FILE *fp) : lzma_strm(ENCODE_LZMA, fp) {}
};

class LZ4F_decoder : public cpr_stream {
public:
	explicit LZ4F_decoder(FILE *fp) : cpr_stream(fp), outbuf(nullptr) {
		LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
	}

	~LZ4F_decoder() override {
		LZ4F_freeDecompressionContext(ctx);
		delete[] outbuf;
	}

	int write(const void *buf, size_t len) override {
		auto ret = len;
		auto inbuf = reinterpret_cast<const uint8_t *>(buf);
		if (!outbuf)
			read_header(inbuf, len);
		size_t read, write;
		LZ4F_errorCode_t code;
		do {
			read = len;
			write = outCapacity;
			code = LZ4F_decompress(ctx, outbuf, &write, inbuf, &read, nullptr);
			if (LZ4F_isError(code)) {
				LOGW("LZ4F decode error: %s\n", LZ4F_getErrorName(code));
				return -1;
			}
			len -= read;
			inbuf += read;
			bwrite(outbuf, write);
		} while (len != 0 || write != 0);
		return ret;
	}

private:
	LZ4F_decompressionContext_t ctx;
	uint8_t *outbuf;
	size_t outCapacity;

	void read_header(const uint8_t *&in, size_t &size) {
		size_t read = size;
		LZ4F_frameInfo_t info;
		LZ4F_getFrameInfo(ctx, &info, in, &read);
		switch (info.blockSizeID) {
			case LZ4F_default:
			case LZ4F_max64KB:  outCapacity = 1 << 16; break;
			case LZ4F_max256KB: outCapacity = 1 << 18; break;
			case LZ4F_max1MB:   outCapacity = 1 << 20; break;
			case LZ4F_max4MB:   outCapacity = 1 << 22; break;
		}
		outbuf = new uint8_t[outCapacity];
		in += read;
		size -= read;
	}
};

class LZ4F_encoder : public cpr_stream {
public:
	explicit LZ4F_encoder(FILE *fp) : cpr_stream(fp), outbuf(nullptr), outCapacity(0) {
		LZ4F_createCompressionContext(&ctx, LZ4F_VERSION);
	}

	~LZ4F_encoder() override {
		destroy();
		LZ4F_freeCompressionContext(ctx);
		delete[] outbuf;
	}

	int write(const void *buf, size_t len) override {
		auto ret = len;
		if (!outbuf)
			write_header();
		if (len == 0)
			return 0;
		auto inbuf = reinterpret_cast<const uint8_t *>(buf);
		size_t read, write;
		do {
			read = len > BLOCK_SZ ? BLOCK_SZ : len;
			write = LZ4F_compressUpdate(ctx, outbuf, outCapacity, inbuf, read, nullptr);
			if (LZ4F_isError(write)) {
				LOGW("LZ4F encode error: %s\n", LZ4F_getErrorName(write));
				return -1;
			}
			len -= read;
			inbuf += read;
			bwrite(outbuf, write);
		} while (len != 0);
		return ret;
	}

protected:
	void finish() override {
		size_t len = LZ4F_compressEnd(ctx, outbuf, outCapacity, nullptr);
		bwrite(outbuf, len);
	}

private:
	LZ4F_compressionContext_t ctx;
	uint8_t *outbuf;
	size_t outCapacity;

	static constexpr size_t BLOCK_SZ = 1 << 22;

	void write_header() {
		LZ4F_preferences_t prefs {
			.autoFlush = 1,
			.compressionLevel = 9,
			.frameInfo = {
				.blockMode = LZ4F_blockIndependent,
				.blockSizeID = LZ4F_max4MB,
				.blockChecksumFlag = LZ4F_noBlockChecksum,
				.contentChecksumFlag = LZ4F_contentChecksumEnabled
			}
		};
		outCapacity = LZ4F_compressBound(BLOCK_SZ, &prefs);
		outbuf = new uint8_t[outCapacity];
		size_t write = LZ4F_compressBegin(ctx, outbuf, outCapacity, &prefs);
		bwrite(outbuf, write);
	}
};

class LZ4_decoder : public cpr_stream {
public:
	explicit LZ4_decoder(FILE *fp)
	: cpr_stream(fp), out_buf(new char[LZ4_UNCOMPRESSED]), buffer(new char[LZ4_COMPRESSED]),
	init(false), block_sz(0), buf_off(0) {}

	~LZ4_decoder() override {
		delete[] out_buf;
		delete[] buffer;
	}

	int write(const void *in, size_t size) override {
		auto ret = size;
		auto inbuf = static_cast<const char *>(in);
		if (!init) {
			// Skip magic
			inbuf += 4;
			size -= 4;
			init = true;
		}
		int write;
		size_t consumed;
		do {
			if (block_sz == 0) {
				block_sz = *((unsigned *) inbuf);
				inbuf += sizeof(unsigned);
				size -= sizeof(unsigned);
			} else if (buf_off + size >= block_sz) {
				consumed = block_sz - buf_off;
				memcpy(buffer + buf_off, inbuf, consumed);
				inbuf += consumed;
				size -= consumed;

				write = LZ4_decompress_safe(buffer, out_buf, block_sz, LZ4_UNCOMPRESSED);
				if (write < 0) {
					LOGW("LZ4HC decompression failure (%d)\n", write);
					return -1;
				}
				bwrite(out_buf, write);

				// Reset
				buf_off = 0;
				block_sz = 0;
			} else {
				// Copy to internal buffer
				memcpy(buffer + buf_off, inbuf, size);
				buf_off += size;
				size = 0;
			}
		} while (size != 0);
		return ret;
	}

private:
	char *out_buf;
	char *buffer;
	bool init;
	unsigned block_sz;
	int buf_off;
};

class LZ4_encoder : public cpr_stream {
public:
	explicit LZ4_encoder(FILE *fp)
	: cpr_stream(fp), outbuf(new char[LZ4_COMPRESSED]), buf(new char[LZ4_UNCOMPRESSED]),
	init(false), buf_off(0), in_total(0) {}

	~LZ4_encoder() override {
		destroy();
		delete[] outbuf;
		delete[] buf;
	}

	int write(const void *in, size_t size) override {
		if (!init) {
			bwrite("\x02\x21\x4c\x18", 4);
			init = true;
		}
		if (size == 0)
			return 0;
		in_total += size;
		const char *inbuf = (const char *) in;
		size_t consumed;
		int write;
		do {
			if (buf_off + size >= LZ4_UNCOMPRESSED) {
				consumed = LZ4_UNCOMPRESSED - buf_off;
				memcpy(buf + buf_off, inbuf, consumed);
				inbuf += consumed;
				size -= consumed;

				write = LZ4_compress_HC(buf, outbuf, LZ4_UNCOMPRESSED, LZ4_COMPRESSED, 9);
				if (write == 0) {
					LOGW("LZ4HC compression failure\n");
					return false;
				}
				bwrite(&write, sizeof(write));
				bwrite(outbuf, write);

				// Reset buffer
				buf_off = 0;
			} else {
				// Copy to internal buffer
				memcpy(buf + buf_off, inbuf, size);
				buf_off += size;
				size = 0;
			}
		} while (size != 0);
		return true;
	}

protected:
	void finish() override {
		if (buf_off) {
			int write = LZ4_compress_HC(buf, outbuf, buf_off, LZ4_COMPRESSED, 9);
			bwrite(&write, sizeof(write));
			bwrite(outbuf, write);
		}
		bwrite(&in_total, sizeof(in_total));
	}

private:
	char *outbuf;
	char *buf;
	bool init;
	int buf_off;
	unsigned in_total;
};

filter_stream *get_encoder(format_t type, FILE *fp) {
	switch (type) {
		case XZ:
			return new xz_encoder(fp);
		case LZMA:
			return new lzma_encoder(fp);
		case BZIP2:
			return new bz_encoder(fp);
		case LZ4:
			return new LZ4F_encoder(fp);
		case LZ4_LEGACY:
			return new LZ4_encoder(fp);
		case GZIP:
		default:
			return new gz_encoder(fp);
	}
}

filter_stream *get_decoder(format_t type, FILE *fp) {
	switch (type) {
		case XZ:
		case LZMA:
			return new lzma_decoder(fp);
		case BZIP2:
			return new bz_decoder(fp);
		case LZ4:
			return new LZ4F_decoder(fp);
		case LZ4_LEGACY:
			return new LZ4_decoder(fp);
		case GZIP:
		default:
			return new gz_decoder(fp);
	}
}

void decompress(char *infile, const char *outfile) {
	bool in_std = infile == "-"sv;
	bool rm_in = false;

	FILE *in_fp = in_std ? stdin : xfopen(infile, "re");
	unique_ptr<stream> strm;

	char buf[4096];
	size_t len;
	while ((len = fread(buf, 1, sizeof(buf), in_fp))) {
		if (!strm) {
			format_t type = check_fmt(buf, len);

			if (!COMPRESSED(type))
				LOGE("Input file is not a supported compressed type!\n");

			fprintf(stderr, "Detected format: [%s]\n", fmt2name[type]);

			/* If user does not provide outfile, infile has to be either
	 		* <path>.[ext], or '-'. Outfile will be either <path> or '-'.
	 		* If the input does not have proper format, abort */

			char *ext = nullptr;
			if (outfile == nullptr) {
				outfile = infile;
				if (!in_std) {
					ext = strrchr(infile, '.');
					if (ext == nullptr || strcmp(ext, fmt2ext[type]) != 0)
						LOGE("Input file is not a supported type!\n");

					// Strip out extension and remove input
					*ext = '\0';
					rm_in = true;
					fprintf(stderr, "Decompressing to [%s]\n", outfile);
				}
			}

			FILE *out_fp = outfile == "-"sv ? stdout : xfopen(outfile, "we");
			strm.reset(get_decoder(type, out_fp));
			if (ext) *ext = '.';
		}
		if (strm->write(buf, len) < 0)
			LOGE("Decompression error!\n");
	}

	strm->close();
	fclose(in_fp);

	if (rm_in)
		unlink(infile);
}

void compress(const char *method, const char *infile, const char *outfile) {
	auto it = name2fmt.find(method);
	if (it == name2fmt.end())
		LOGE("Unknown compression method: [%s]\n", method);

	bool in_std = infile == "-"sv;
	bool rm_in = false;

	FILE *in_fp = in_std ? stdin : xfopen(infile, "re");
	FILE *out_fp;

	if (outfile == nullptr) {
		if (in_std) {
			out_fp = stdout;
		} else {
			/* If user does not provide outfile and infile is not
			 * STDIN, output to <infile>.[ext] */
			string tmp(infile);
			tmp += fmt2ext[it->second];
			out_fp = xfopen(tmp.data(), "we");
			fprintf(stderr, "Compressing to [%s]\n", tmp.data());
			rm_in = true;
		}
	} else {
		out_fp = outfile == "-"sv ? stdout : xfopen(outfile, "we");
	}

	unique_ptr<stream> strm(get_encoder(it->second, out_fp));

	char buf[4096];
	size_t len;
	while ((len = fread(buf, 1, sizeof(buf), in_fp))) {
		if (strm->write(buf, len) < 0)
			LOGE("Compression error!\n");
	};

	strm->close();
	fclose(in_fp);

	if (rm_in)
		unlink(infile);
}
