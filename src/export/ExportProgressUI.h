/**********************************************************************

  Audacity: A Digital Audio Editor

  ExportProgressUI.h

  Vitaly Sverchinsky

**********************************************************************/

#pragma once

#include <future>

#include "Export.h"
#include "ExportTypes.h"
#include "BasicUI.h"
#include "ExportPlugin.h"
#include "wxFileNameWrapper.h"

class ExportPluginDelegate;
class Exporter;

namespace ExportProgressUI
{
   ExportResult Show(ExportTask exportTask);

   template<typename Callable>
   void ExceptionWrappedCall(Callable callable)
   {
      try
      {
         callable();
      }
      catch(ExportDiskFullError& e)
      {
         ShowDiskFullExportErrorDialog(e.GetFileName());
      }
      catch(ExportErrorException& e)
      {
         ShowExportErrorDialog(e.GetMessage(),
            XO("Warning"),
            e.GetHelpPageId(),
            true);
      }
      catch(ExportException& e)
      {
         BasicUI::ShowMessageBox(TranslatableString { e.What(), {}});
      }
      catch(...)
      {
         BasicUI::ShowMessageBox(XO("Export error"));
      }
   }

   

}
