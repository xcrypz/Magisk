#include <utils.h>
#include <logging.h>
#include <stream.h>

static int strm_read(void *v, char *buf, int len) {
	auto strm = reinterpret_cast<stream *>(v);
	return strm->read(buf, len);
}

static int strm_write(void *v, const char *buf, int len) {
	auto strm = reinterpret_cast<stream *>(v);
	return strm->write(buf, len);
}

static fpos_t strm_seek(void *v, fpos_t off, int whence) {
	auto strm = reinterpret_cast<stream *>(v);
	return strm->seek(off, whence);
}

static int strm_close(void *v) {
	auto strm = reinterpret_cast<stream *>(v);
	int ret = strm->close();
	delete strm;
	return ret;
}

FILE *open_stream(stream *strm) {
	FILE *fp = funopen(strm, strm_read, strm_write, strm_seek, strm_close);
	// Disable buffering
	setbuf(fp, nullptr);
	return fp;
}

int stream::read(void *buf, size_t len)  {
	LOGE("This stream does not support read");
	return -1;
}

int stream::write(const void *buf, size_t len) {
	LOGE("This stream does not support write");
	return -1;
}

off_t stream::seek(off_t off, int whence) {
	LOGE("This stream does not support seek");
	return -1;
}

int stream::close() {
	return 0;
}

int filter_stream::read(void *buf, size_t len) {
	return fread(buf, len, 1, fp);
}

int filter_stream::write(const void *buf, size_t len) {
	return fwrite(buf, len, 1, fp);
}

int filter_stream::close() {
	int ret = fclose(fp);
	fp = nullptr;
	return ret;
}

void filter_stream::set_base(FILE *f) {
	if (fp) fclose(fp);
	fp = f;
}

off_t seekable_stream::seek_pos(off_t off, int whence) {
	switch (whence) {
		case SEEK_CUR:
			return _pos + off;
		case SEEK_END:
			return end_pos() + off;
		case SEEK_SET:
			return off;
		default:
			return -1;
	}
}

byte_stream::byte_stream(uint8_t *&buf, size_t &len) : _buf(buf), _len(len) {
	buf = nullptr;
	len = 0;
}

int byte_stream::read(void *buf, size_t len) {
	len = std::min(len, _len - _pos);
	memcpy(buf, _buf + _pos, len);
	return len;
}

int byte_stream::write(const void *buf, size_t len) {
	resize(_pos + len);
	memcpy(_buf + _pos, buf, len);
	_pos += len;
	_len = std::max(_len, _pos);
	return len;
}

off_t byte_stream::seek(off_t off, int whence) {
	off_t np = seek_pos(off, whence);
	if (np < 0)
		return -1;
	resize(np, true);
	_pos = np;
	return np;
}

void byte_stream::resize(size_t new_pos, bool zero) {
	bool resize = false;
	size_t old_cap = _cap;
	while (new_pos > _cap) {
		_cap = _cap ? (_cap << 1) - (_cap >> 1) : 1 << 12;
		resize = true;
	}
	if (resize) {
		_buf = (uint8_t *) xrealloc(_buf, _cap);
		if (zero)
			memset(_buf + old_cap, 0, _cap - old_cap);
	}
}

int fd_stream::read(void *buf, size_t len) {
	return ::read(fd, buf, len);
}

int fd_stream::write(const void *buf, size_t len) {
	return ::write(fd, buf, len);
}

off_t fd_stream::seek(off_t off, int whence) {
	return lseek(fd, off, whence);
}
