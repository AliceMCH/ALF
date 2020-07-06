// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file Swt.cxx
/// \brief Definition of SWT operations
///
/// \author Kostas Alexopoulos (kostas.alexopoulos@cern.ch)

#include <boost/format.hpp>
#include <chrono>
#include <thread>

#include "Alf/Exception.h"
#include "Logger.h"
#include "ReadoutCard/CardDescriptor.h"
#include "ReadoutCard/CardFinder.h"
#include "ReadoutCard/ChannelFactory.h"
#include "ReadoutCard/Cru.h"
#include "Alf/Swt.h"

namespace o2
{
namespace alf
{

namespace sc_regs = AliceO2::roc::Cru::ScRegisters;

Swt::Swt(AlfLink link) : mBar2(link.bar), mLink(link)
{
  setChannel(mLink.linkId); // TODO: Decouple this from the class?
  mLlaSession = std::make_unique<LlaSession>("DDT", link.cardSequence);
}

Swt::Swt(const roc::Parameters::CardIdType& cardId, int linkId)
{
  init(cardId, linkId);
}

Swt::Swt(std::string cardId, int linkId)
{
  init(roc::Parameters::cardIdFromString(cardId), linkId);
}

void Swt::init(const roc::Parameters::CardIdType& cardId, int linkId)
{
  auto card = roc::findCard(cardId); //TODO: Use it elsewhere??

  mBar2 = roc::ChannelFactory().getBar(card.pciAddress, 2);

  mLink = AlfLink{
    "DDT", //TODO: From session?
    card.sequenceId,
    linkId,
    mBar2,
    roc::CardType::Cru
  };

  mLlaSession = std::make_unique<LlaSession>("DDT", card.sequenceId);

  setChannel(mLink.linkId);
}

void Swt::setChannel(int gbtChannel)
{
  barWrite(sc_regs::SC_LINK.index, gbtChannel);
}

void Swt::reset()
{
  barWrite(sc_regs::SC_RESET.index, 0x1);
  barWrite(sc_regs::SC_RESET.index, 0x0); //void cmd to sync clocks
  mWordSequence = 0;
}

std::vector<SwtWord> Swt::read(SwtWord::Size wordSize, TimeOut msTimeOut)
{
  std::vector<SwtWord> words;
  uint32_t numWords = 0x0;

  auto timeOut = std::chrono::steady_clock::now() + std::chrono::milliseconds(msTimeOut);
  while ((std::chrono::steady_clock::now() < timeOut) && (numWords < 1)) {
    numWords = (barRead(sc_regs::SWT_MON.index) >> 16);
  }

  if (numWords < 1) { // #WORDS in READ FIFO
    BOOST_THROW_EXCEPTION(SwtException() << ErrorInfo::Message("Not enough words in SWT READ FIFO"));
  }

  for (int i = 0; i < (int)numWords; i++) {
    SwtWord tempWord;

    barWrite(sc_regs::SWT_CMD.index, 0x2);
    barWrite(sc_regs::SWT_CMD.index, 0x0); // void cmd to sync clocks

    tempWord.setLow(barRead(sc_regs::SWT_RD_WORD_L.index));

    if (wordSize == SwtWord::Size::Medium || wordSize == SwtWord::Size::High) {
      tempWord.setMed(barRead(sc_regs::SWT_RD_WORD_M.index));
    }

    if (wordSize == SwtWord::Size::High) { // Sequence set with high part of the word
      tempWord.setHigh(barRead(sc_regs::SWT_RD_WORD_H.index));
    } else { // Only set sequence if smaller word requested
      tempWord.setSequence(barRead(sc_regs::SWT_RD_WORD_H.index));
    }

    mWordSequence = (mWordSequence + 1) % 16;

    // If we get the same counter as before it means the FIFO wasn't updated; drop the word
    if (tempWord.getSequence() != mWordSequence) {

      if (std::chrono::steady_clock::now() > timeOut) {
        BOOST_THROW_EXCEPTION(SwtException() << ErrorInfo::Message("Timed out: not enough words in SWT READ FIFO"));
      }

      getWarningLogger() << "SWT word sequence duplicate" << endm;

      mWordSequence = tempWord.getSequence(); //roll mWordSequence back by one
      numWords++;                             //will have to read one more time
      continue;
    }

    //wordMonPairs.push_back(std::make_pair(tempWord, barRead(sc_regs::SWT_MON.index)));
    words.push_back(tempWord);
  }

  return words;
}

void Swt::write(const SwtWord& swtWord)
{
  // prep the swt word
  if (swtWord.getSize() == SwtWord::Size::High) {
    barWrite(sc_regs::SWT_WR_WORD_H.index, swtWord.getHigh());
  }
  if (swtWord.getSize() == SwtWord::Size::High || swtWord.getSize() == SwtWord::Size::Medium) {
    barWrite(sc_regs::SWT_WR_WORD_M.index, swtWord.getMed());
  }
  barWrite(sc_regs::SWT_WR_WORD_L.index, swtWord.getLow()); // The LOW bar write, triggers the write operation

  //return barRead(sc_regs::SWT_MON.index);
}

void Swt::barWrite(uint32_t index, uint32_t data)
{
  mBar2->writeRegister(index, data);
}

uint32_t Swt::barRead(uint32_t index)
{
  uint32_t read = mBar2->readRegister(index);
  return read;
}

std::vector<std::pair<Swt::Operation, Swt::Data>> Swt::executeSequence(std::vector<std::pair<Operation, Data>> sequence, bool lock)
{
  if (lock) {
    mLlaSession->start();
  }

  std::vector<std::pair<Operation, Data>> ret;

  for (const auto& it : sequence) {
    Operation operation = it.first;
    Data data = it.second;
    try {
      if (operation == Operation::Read) {
        int timeOut;
        try {
          timeOut = boost::get<TimeOut>(data);
        } catch (...) { // no timeout was provided
          data = DEFAULT_SWT_TIMEOUT_MS;
          timeOut = boost::get<TimeOut>(data);
        }
        auto results = read(SwtWord::Size::Low, timeOut);

        for (const auto& result : results) {
          ret.push_back({ Operation::Read, result });
        }
      } else if (operation == Operation::Write) {
        SwtWord word = boost::get<SwtWord>(data);
        write(word);
        ret.push_back({ Operation::Write, word });
        //ret.push_back({ Operation::Write, {} }); // TODO: Is it better to return {} ?
      } else if (operation == Operation::Reset) {
        reset();
        ret.push_back({ Operation::Reset, {} });
      } else {
        BOOST_THROW_EXCEPTION(SwtException() << ErrorInfo::Message("SWT operation type unknown"));
      }
    } catch (const SwtException& e) {
      std::string meaningfulMessage;
      if (operation == Operation::Read) {
        meaningfulMessage = (boost::format("SWT_SEQUENCE READ timeout=%d cardSequence=%d link=%d, error='%s'") % boost::get<TimeOut>(data) % mLink.cardSequence % mLink.linkId % e.what()).str();
      } else if (operation == Operation::Write) {
        meaningfulMessage = (boost::format("SWT_SEQUENCE WRITE data=%s cardSequence=%d link=%d, error='%s'") % boost::get<SwtWord>(data) % mLink.cardSequence % mLink.linkId % e.what()).str();
      } else if (operation == Operation::Reset) {
        meaningfulMessage = (boost::format("SWT_SEQUENCE RESET cardSequence=%d link=%d, error='%s'") % mLink.cardSequence % mLink.linkId % e.what()).str();
      } else {
        meaningfulMessage = (boost::format("SWT_SEQUENCE UNKNOWN cardSequence=%d link=%d,  error='%s'") % mLink.cardSequence % mLink.linkId % e.what()).str();
      }
      getErrorLogger() << meaningfulMessage << endm;

      ret.push_back({ Operation::Error, meaningfulMessage });
      break;
    }
  }

  if (lock) {
    mLlaSession->stop();
  }

  return ret;
}

std::string Swt::writeSequence(std::vector<std::pair<Operation, Data>> sequence, bool lock)
{
  std::stringstream resultBuffer;
  auto out = executeSequence(sequence, lock);
  for (const auto& it : out) {
    Operation operation = it.first;
    Data data = it.second;
    if (operation == Operation::Read) {
      resultBuffer << data << "\n";
    } else if (operation == Operation::Write) {
      resultBuffer << "0\n";
    } else if (operation == Operation::Reset) {
      /* DO NOTHING */
    } else if (operation == Operation::Error) {
      resultBuffer << data;
      getErrorLogger() << data << endm;
      BOOST_THROW_EXCEPTION(SwtException() << ErrorInfo::Message(resultBuffer.str()));
      break;
    }
  }

  return resultBuffer.str();
}

} // namespace alf
} // namespace o2