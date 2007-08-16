// rTorrent - BitTorrent client
// Copyright (C) 2005-2007, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// In addition, as a special exception, the copyright holders give
// permission to link the code of portions of this program with the
// OpenSSL library under certain conditions as described in each
// individual source file, and distribute linked combinations
// including the two.
//
// You must obey the GNU General Public License in all respects for
// all of the code used other than OpenSSL.  If you modify file(s)
// with this exception, you may extend this exception to your version
// of the file(s), but you are not obligated to do so.  If you do not
// wish to do so, delete this exception statement from your version.
// If you delete this exception statement from all source files in the
// program, then also delete it here.
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#include "config.h"

#include <rak/error_number.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <torrent/exceptions.h>
#include <torrent/poll.h>

#include "utils/socket_fd.h"

#include "control.h"
#include "globals.h"
#include "scgi.h"

// Test:
// #include "core/manager.h"
// #include <rak/timer.h>

// static rak::timer scgiTimer;

namespace rpc {

void
SCgiTask::open(SCgi* parent, int fd) {
  m_parent   = parent;
  m_fileDesc = fd;
  m_buffer   = new char[(m_bufferSize = default_buffer_size) + 1];
  m_position = m_buffer;
  m_body     = NULL;

  control->poll()->open(this);
  control->poll()->insert_read(this);
  control->poll()->insert_error(this);

//   scgiTimer = rak::timer::current();
}

void
SCgiTask::close() {
  if (!get_fd().is_valid())
    return;

  control->poll()->remove_read(this);
  control->poll()->remove_write(this);
  control->poll()->remove_error(this);
  control->poll()->close(this);

  get_fd().close();
  get_fd().clear();

  delete [] m_buffer;
  m_buffer = NULL;

  // Test
//   char buffer[512];
//   sprintf(buffer, "SCgi system call processed: %i", (int)(rak::timer::current() - scgiTimer).usec());
//   control->core()->push_log(std::string(buffer));
}

void
SCgiTask::event_read() {
  int bytes = ::recv(m_fileDesc, m_position, m_bufferSize - (m_position - m_buffer), 0);

  if (bytes == -1) {
    if (!rak::error_number::current().is_blocked_momentary())
      close();

    return;
  }

  // The buffer has space to nul-terminate to ease the parsing below.
  m_position += bytes;
  *m_position = '\0';

  if (m_body == NULL) {
    // Don't bother caching the parsed values, as we're likely to
    // receive all the data we need the first time.
    char* current;

    int contentSize;
    int headerSize = strtol(m_buffer, &current, 0);

    if (current == m_buffer || current == m_position)
      return;

    if (*current != ':' || headerSize < 17 || headerSize > max_header_size)
      goto event_read_failed;

    if (std::distance(++current, m_position) < headerSize + 1)
      return;

    if (std::memcmp(current, "CONTENT_LENGTH", 15) != 0)
      goto event_read_failed;

    char* contentPos;
    contentSize = strtol(current + 15, &contentPos, 0);

    if (*contentPos != '\0' || contentSize <= 0 || contentSize > max_content_size)
      goto event_read_failed;

    m_body = current + headerSize + 1;
    headerSize = std::distance(m_buffer, m_body);

    if ((unsigned int)(contentSize + headerSize) < m_bufferSize) {
      m_bufferSize = contentSize + headerSize;

    } else if ((unsigned int)(contentSize + headerSize) <= default_buffer_size) {
      m_bufferSize = contentSize;

      std::memmove(m_buffer, m_body, std::distance(m_body, m_position));
      m_position = m_buffer + std::distance(m_body, m_position);
      m_body = m_buffer;

    } else {
      char* tmp = new char[(m_bufferSize = contentSize)];
      std::memcpy(tmp, m_body, std::distance(m_body, m_position));
      delete [] m_buffer;

      m_position = tmp + std::distance(m_body, m_position);
      m_buffer = tmp;
      m_body = tmp;
    }
  }

  if ((unsigned int)std::distance(m_buffer, m_position) != m_bufferSize)
    return;

  control->poll()->remove_read(this);
  control->poll()->insert_write(this);

  // Close if the call failed, else stay open to write back data.
  if (!m_parent->receive_call(this, m_body, m_bufferSize - std::distance(m_buffer, m_body)))
    close();

  return;

 event_read_failed:
  throw torrent::internal_error("SCgiTask::event_read() fault not handled.");
}

void
SCgiTask::event_write() {
  int bytes = ::send(m_fileDesc, m_position, m_bufferSize, 0);

  if (bytes == -1) {
    if (!rak::error_number::current().is_blocked_momentary())
      close();

    return;
  }

  m_position += bytes;
  m_bufferSize -= bytes;

  if (bytes == 0 || m_bufferSize == 0)
    return close();
}

void
SCgiTask::event_error() {
  close();
}

bool
SCgiTask::receive_write(const char* buffer, uint32_t length) {
  // Need to cast due to a bug in MacOSX gcc-4.0.1.
  if (length + 256 > std::max(m_bufferSize, (unsigned int)default_buffer_size)) {
    delete [] m_buffer;
    m_buffer = new char[length + 256];
  }

  // Who ever bothers to check the return value?
  int headerSize = sprintf(m_buffer, "Status: 200 OK\r\nContent-Type: text/xml\r\nContent-Length: %i\r\n\r\n", length);

  m_position = m_buffer;
  m_bufferSize = length + headerSize;
  
  std::memcpy(m_buffer + headerSize, buffer, length);
  event_write();

  return true;
}

}