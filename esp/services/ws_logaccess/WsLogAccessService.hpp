/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2021 HPCC Systems.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#ifndef _WsLOGACCESS_HPP_
#define _WsLOGACCESS_HPP_

#include "ws_logaccess_esp.ipp"

class Cws_logaccessEx : public Cws_logaccess
{
public:

    Cws_logaccessEx();
    virtual ~Cws_logaccessEx();
    virtual bool onGetLogAccessInfo(IEspContext &context, IEspGetLogAccessInfoRequest &req, IEspGetLogAccessInfoResponse &resp);
    virtual bool onGetLogs(IEspContext &context, IEspGetLogsRequest &req, IEspGetLogsResponse & resp);
    virtual bool onGetHealthReport(IEspContext &context, IEspGetHealthReportRequest &req, IEspGetHealthReportResponse &resp);
};

#endif
