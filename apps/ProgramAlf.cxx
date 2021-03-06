// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file ProgramAlf.cxx
/// \brief Definition of the command line tool to run the ALF server
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)
/// \author Kostas Alexopoulos (kostas.alexopoulos@cern.ch)

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/asio.hpp>
#include <cstdlib>

#include "AlfServer.h"
#include "Common/Program.h"
#include "DimServices/ServiceNames.h"
#include "Logger.h"
#include "ReadoutCard/CardDescriptor.h"
#include "ReadoutCard/CardFinder.h"
#include "ReadoutCard/ChannelFactory.h"

namespace ip = boost::asio::ip;
namespace po = boost::program_options;

namespace AliceO2
{
namespace Alf
{

AliceO2::InfoLogger::InfoLogger logger;

class ProgramAlf : public AliceO2::Common::Program
{
 public:
  ProgramAlf()
  {
  }

  virtual Description getDescription() override
  {
    return { "ALF", "ALICE Low-level Front-end DIM server", "o2-alf" };
  }

  virtual void addOptions(po::options_description& options) override
  {
    options.add_options()("dim-dns-node",
                          po::value<std::string>(&mOptions.dimDnsNode)->default_value(""),
                          "The DIM DNS node to set the env var if not already set");
  }

  virtual void run(const po::variables_map&) override
  {
    //verbose = isVerbose();

    getLogger() << "ALF server initializations..." << endm;

    if (mOptions.dimDnsNode != "") {
      getLogger() << "Setting DIM_DNS_NODE from argument." << endm;
      getLogger() << "DIM_DNS_NODE=" << mOptions.dimDnsNode << endm;
    } else if (const char* dimDnsNode = std::getenv("DIM_DNS_NODE")) {
      getLogger() << "Picked up DIM_DMS_NODE from the environment." << endm;
      getLogger() << "DIM_DNS_NODE=" << dimDnsNode << endm;
      mOptions.dimDnsNode = dimDnsNode;
    } else {
      BOOST_THROW_EXCEPTION(AlfException() << ErrorInfo::Message("DIM_DNS_NODE env variable not set, and no relevant argument provided.")); // InfoLogger and errors?
    }

    std::string alfId = ip::host_name();
    boost::to_upper(alfId);

    getLogger() << "Starting the DIM Server" << endm;
    DimServer::setDnsNode(mOptions.dimDnsNode.c_str(), 2505);
    DimServer::start(("ALF_" + alfId).c_str());

    AlfServer alfServer = AlfServer();

    std::vector<roc::CardDescriptor> cardsFound = roc::findCards();
    int fakeSerial = 0;
    for (auto const& card : cardsFound) {
      std::vector<AlfLink> links;

      std::shared_ptr<roc::BarInterface> bar2;

      // Make the RPC services for every card & link
      if (card.cardType == roc::CardType::Cru) { //TODO: To be deprecated when findCards supports types
                                                 //TODO: What about CRORC ????????

        //auto serialMaybe = card.serialNumber.get();
        //int serial = serialMaybe ? serialMaybe : fakeSerial++;
        int serial = fakeSerial++;

        getLogger() << "Card #" << serial << " : " << card.pciAddress << endm;
        bar2 = roc::ChannelFactory().getBar(card.pciAddress, 2);
        for (int linkId = 0; linkId < CRU_NUM_LINKS; linkId++) {
          links.push_back({ alfId, serial, linkId, bar2 });
        }

      } else {
        getLogger() << InfoLogger::InfoLogger::Severity::Warning << card.pciAddress << " is not a CRU. Skipping..." << endm;
      }

      if (isVerbose()) {
        for (auto const& link : links) {
          getLogger() << link.alfId << " " << link.serial << " " << link.linkId << endm;
        }
      }

      alfServer.makeRpcServers(links);
    }

    // main thread
    while (!isSigInt()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

 private:
  struct OptionsStruct {
    std::string dimDnsNode = "";
  } mOptions;
};

} // namespace Alf
} // namespace AliceO2

int main(int argc, char** argv)
{
  return AliceO2::Alf::ProgramAlf().execute(argc, argv);
}
