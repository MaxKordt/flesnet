// Copyright 2013 Jan de Cuveland <cmail@cuveland.de>
/// \file
/// \brief Defines the fles::TimesliceView class.
#pragma once

#include "Timeslice.hpp"
#include "TimesliceCompletion.hpp"
#include "TimesliceWorkItem.hpp"
#include <boost/interprocess/ipc/message_queue.hpp>
#include <cstdint>
#include <memory>

namespace fles {

/**
 * \brief The TimesliceView class provides access to the data of a single
 * timeslice in memory.
 */
class TimesliceView : public Timeslice {
public:
  /// Delete copy constructor (non-copyable).
  TimesliceView(const TimesliceView&) = delete;
  /// Delete assignment operator (non-copyable).
  void operator=(const TimesliceView&) = delete;

  ~TimesliceView() override;

private:
  friend class TimesliceReceiver;
  friend class StorableTimeslice;

  TimesliceView(
      TimesliceWorkItem work_item,
      uint8_t* data,
      TimesliceComponentDescriptor* desc,
      std::shared_ptr<boost::interprocess::message_queue> completions_mq);

  TimesliceCompletion completion_ = TimesliceCompletion();

  std::shared_ptr<boost::interprocess::message_queue> completions_mq_;
};

} // namespace fles
