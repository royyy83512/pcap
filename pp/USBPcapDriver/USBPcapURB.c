/*
 * Copyright (c) 2013-2019 Tomasz Moń <desowin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include "USBPcapMain.h"
#include "USBPcapURB.h"
#include "USBPcapTables.h"
#include "USBPcapBuffer.h"
#include "USBPcapHelperFunctions.h"

#include <stddef.h> /* Required for offsetof macro */

#if DBG
VOID USBPcapPrintChars(PCHAR text, PUCHAR buffer, ULONG length)
{
    ULONG i;

    KdPrint(("%s HEX: ", text));
    for (i = 0; i < length; ++i)
    {
        KdPrint(("%02X ", buffer[i]));
    }

    KdPrint(("\n%s TEXT: ", text));
    for (i = 0; i < length; ++i)
    {
        /*
         * For printable characters, print the character,
         * otherwise print dot
         */
        if (buffer[i] >= 0x20 && buffer[i] <= 0x7E)
        {
            KdPrint(("%c", buffer[i]));
        }
        else
        {
            KdPrint(("."));
        }
    }

    KdPrint(("\n"));
}
#else
#define USBPcapPrintChars(text, buffer, length) {}
#endif

static PVOID USBPcapURBGetBufferPointer(ULONG length,
                                        PVOID buffer,
                                        PMDL  bufferMDL)
{
    ASSERT((length == 0) ||
           ((length != 0) && (buffer != NULL || bufferMDL != NULL)));

    if (length == 0)
    {
        return NULL;
    }
    else if (buffer != NULL)
    {
        return buffer;
    }
    else if (bufferMDL != NULL)
    {
        PVOID address = MmGetSystemAddressForMdlSafe(bufferMDL,
                                                     NormalPagePriority);
        return address;
    }
    else
    {
        DkDbgStr("Invalid buffer!");
        return NULL;
    }
}

static VOID
USBPcapParseInterfaceInformation(PUSBPCAP_DEVICE_DATA pDeviceData,
                                 PUSBD_INTERFACE_INFORMATION pInterface,
                                 USHORT interfaces_len)
{
    ULONG i, j;
    KIRQL irql;

    /*
     * Iterate over all interfaces in search for pipe handles
     * Add endpoint information to endpoint table
     */
    i = 0;
    while (interfaces_len != 0 && pInterface->Length != 0)
    {
        PUSBD_PIPE_INFORMATION Pipe = pInterface->Pipes;

        if (interfaces_len < sizeof(USBD_INTERFACE_INFORMATION))
        {
            /* There is no enough bytes to hold USBD_INTERFACE_INFORMATION.
             * Stop parsing.
             */
            KdPrint(("Remaining %d bytes of interfaces not parsed.\n",
                     interfaces_len));
            break;
        }

        if (pInterface->Length > interfaces_len)
        {
            /* Interface expands beyond URB, don't try to parse it. */
            KdPrint(("Interface length: %d. Remaining bytes: %d. "
                     "Parsing stopped.\n",
                     pInterface->Length, interfaces_len));
            break;
        }

        /* At this point if NumberOfPipes is either 0 or 1 we can proceed
         * as sizeof(USBD_INTERFACE_INFORMATION) covers interface
         * information together with one pipe information.
         *
         * Perform additional sanity check if there is more than one pipe in
         * the interface.
         */
        if (pInterface->NumberOfPipes > 1)
        {
            ULONG required_length;
            required_length = sizeof(USBD_INTERFACE_INFORMATION) +
                              ((pInterface->NumberOfPipes - 1) *
                               sizeof(USBD_PIPE_INFORMATION));

            if (interfaces_len < required_length)
            {
                KdPrint(("%d pipe information does not fit in %d bytes.",
                         pInterface->NumberOfPipes, interfaces_len));
                break;
            }
        }

        /* End of sanity checks, parse pipe information. */
        KdPrint(("Interface %d Len: %d Class: %02x Subclass: %02x"
                 "Protocol: %02x Number of Pipes: %d\n",
                 i, pInterface->Length, pInterface->Class,
                 pInterface->SubClass, pInterface->Protocol,
                 pInterface->NumberOfPipes));

        for (j=0; j<pInterface->NumberOfPipes; ++j, Pipe++)
        {
            KdPrint(("Pipe %d MaxPacketSize: %d"
                    "EndpointAddress: %d PipeType: %d"
                    "PipeHandle: %02x\n",
                    j,
                    Pipe->MaximumPacketSize,
                    Pipe->EndpointAddress,
                    Pipe->PipeType,
                    Pipe->PipeHandle));

            KeAcquireSpinLock(&pDeviceData->tablesSpinLock,
                              &irql);
            USBPcapAddEndpointInfo(pDeviceData->endpointTable,
                                   Pipe,
                                   pDeviceData->deviceAddress);
            KeReleaseSpinLock(&pDeviceData->tablesSpinLock,
                              irql);
        }

        /* Advance to next interface */
        i++;
        interfaces_len -= pInterface->Length;
        pInterface = (PUSBD_INTERFACE_INFORMATION)
                         ((PUCHAR)pInterface + pInterface->Length);
    }
}

__inline static VOID
USBPcapAnalyzeControlTransfer(struct _URB_CONTROL_TRANSFER* transfer,
                              struct _URB_HEADER* header,
                              PUSBPCAP_DEVICE_DATA pDeviceData,
                              PIRP pIrp,
                              BOOLEAN post)
{
    BOOLEAN                        transferFromDevice;
    USBPCAP_BUFFER_CONTROL_HEADER  packetHeader;
    PVOID                          dataBuffer;
    UINT32                         dataBufferLength;

    if (transfer->TransferFlags & USBD_TRANSFER_DIRECTION_IN)
    {
        /* From device to host */
        transferFromDevice = TRUE;
    }
    else
    {
        /* From host to device */
        transferFromDevice = FALSE;
    }

    packetHeader.header.headerLen = sizeof(USBPCAP_BUFFER_CONTROL_HEADER);
    packetHeader.header.irpId     = (UINT64) pIrp;
    packetHeader.header.status    = header->Status;
    packetHeader.header.function  = header->Function;
    packetHeader.header.info      = 0;
    if (post == TRUE)
    {
        packetHeader.header.info |= USBPCAP_INFO_PDO_TO_FDO;
    }

    packetHeader.header.bus      = pDeviceData->pRootData->busId;
    packetHeader.header.device   = pDeviceData->deviceAddress;

    packetHeader.header.endpoint = 0;
    if ((transfer->TransferFlags & USBD_DEFAULT_PIPE_TRANSFER) ||
        (transfer->PipeHandle == NULL))
    {
        /* Transfer to default control endpoint 0 */
    }
    else
    {
        USBPCAP_ENDPOINT_INFO                   info;
        BOOLEAN                                 epFound;

        epFound = USBPcapRetrieveEndpointInfo(pDeviceData,
                                              transfer->PipeHandle,
                                              &info);
        if (epFound == TRUE)
        {
            packetHeader.header.endpoint = info.endpointAddress;
        }
    }

    if (transferFromDevice)
    {
        packetHeader.header.endpoint |= 0x80;
    }

    packetHeader.header.transfer = USBPCAP_TRANSFER_CONTROL;

    if (transfer->TransferBufferLength != 0)
    {
        dataBuffer =
            USBPcapURBGetBufferPointer(transfer->TransferBufferLength,
                                       transfer->TransferBuffer,
                                       transfer->TransferBufferMDL);
        dataBufferLength = (UINT32)transfer->TransferBufferLength;
    }
    else
    {
        dataBuffer = NULL;
        dataBufferLength = 0;
    }

    /* Add Setup stage to log only when on its way from FDO to PDO. */
    if (post == FALSE)
    {
        USBPCAP_PAYLOAD_ENTRY  payload[3];

        packetHeader.header.dataLength = 8;
        packetHeader.stage = USBPCAP_CONTROL_STAGE_SETUP;

        payload[0].size   = 8;
        payload[0].buffer = (PVOID)&transfer->SetupPacket[0];
        payload[1].size   = 0;
        payload[1].buffer = NULL;
        payload[2].size   = 0;
        payload[2].buffer = NULL;

        if (!transferFromDevice)
        {
            packetHeader.header.dataLength += dataBufferLength;
            payload[1].size = dataBufferLength;
            payload[1].buffer = dataBuffer;
        }

        USBPcapBufferWritePayload(pDeviceData->pRootData,
                                 (PUSBPCAP_BUFFER_PACKET_HEADER)&packetHeader,
                                 payload);
    }

    /* Add Complete stage to log when on its way from PDO to FDO */
    if (post == TRUE)
    {
        USBPCAP_PAYLOAD_ENTRY  payload[2];

        packetHeader.header.dataLength = 0;
        packetHeader.stage = USBPCAP_CONTROL_STAGE_COMPLETE;

        payload[0].size   = 0;
        payload[0].buffer = NULL;
        payload[1].size   = 0;
        payload[1].buffer = NULL;

        if (transferFromDevice)
        {
            packetHeader.header.dataLength += dataBufferLength;
            payload[0].size = dataBufferLength;
            payload[0].buffer = dataBuffer;
        }

        USBPcapBufferWritePayload(pDeviceData->pRootData,
                                 (PUSBPCAP_BUFFER_PACKET_HEADER)&packetHeader,
                                 payload);
    }
}

/*
 * Analyzes the URB
 *
 * post is FALSE when the request is being on its way to the bus driver
 * post is TRUE when the request returns from the bus driver
 */
VOID USBPcapAnalyzeURB(PIRP pIrp, PURB pUrb, BOOLEAN post,
                       PUSBPCAP_DEVICE_DATA pDeviceData)
{
    struct _URB_HEADER     *header;
    USBPCAP_URB_IRP_INFO    unknownURBSubmitInfo;
    BOOLEAN                 hasUnknownURBSubmitInfo;

    ASSERT(pUrb != NULL);
    ASSERT(pDeviceData != NULL);
    ASSERT(pDeviceData->pRootData != NULL);

    header = (struct _URB_HEADER*)pUrb;

    /* Check if the IRP on its way from FDO to PDO had unknown URB function */
    if (post)
    {
        hasUnknownURBSubmitInfo =
            USBPcapObtainURBIRPInfo(pDeviceData, pIrp, &unknownURBSubmitInfo);
    }
    else
    {
        hasUnknownURBSubmitInfo = FALSE;
    }

    /* Following URBs are always analyzed */
    switch (header->Function)
    {
        case URB_FUNCTION_SELECT_CONFIGURATION:
        {
            struct _URB_SELECT_CONFIGURATION *pSelectConfiguration;
            USHORT interfaces_len;

            if (post == FALSE)
            {
                /* Pass the request to host controller,
                 * we are interested only in select configuration
                 * after the fields are set by host controller driver */
                break;
            }

            DkDbgStr("URB_FUNCTION_SELECT_CONFIGURATION");
            pSelectConfiguration = (struct _URB_SELECT_CONFIGURATION*)pUrb;

            /* Check if there is interface information in the URB */
            if (pUrb->UrbHeader.Length > offsetof(struct _URB_SELECT_CONFIGURATION, Interface))
            {
                /* Calculate interfaces length */
                interfaces_len = pUrb->UrbHeader.Length;
                interfaces_len -= offsetof(struct _URB_SELECT_CONFIGURATION, Interface);

                KdPrint(("Header Len: %d Interfaces_len: %d\n",
                        pUrb->UrbHeader.Length, interfaces_len));

                USBPcapParseInterfaceInformation(pDeviceData,
                                                 &pSelectConfiguration->Interface,
                                                 interfaces_len);
            }

            /* Store the configuration information for later use */
            if (pDeviceData->descriptor != NULL)
            {
                ExFreePool((PVOID)pDeviceData->descriptor);
            }

            if (pSelectConfiguration->ConfigurationDescriptor != NULL)
            {
                SIZE_T descSize = pSelectConfiguration->ConfigurationDescriptor->wTotalLength;

                pDeviceData->descriptor =
                    ExAllocatePoolWithTag(NonPagedPool,
                                          descSize,
                                          (ULONG)'CSED');

                RtlCopyMemory(pDeviceData->descriptor,
                              pSelectConfiguration->ConfigurationDescriptor,
                              (SIZE_T)descSize);
            }
            else
            {
                pDeviceData->descriptor = NULL;
            }

            break;
        }

        case URB_FUNCTION_SELECT_INTERFACE:
        {
            struct _URB_SELECT_INTERFACE *pSelectInterface;
            USHORT interfaces_len;

            if (post == FALSE)
            {
                /* Pass the request to host controller,
                 * we are interested only in select interface
                 * after the fields are set by host controller driver */
                break;
            }

            DkDbgStr("URB_FUNCTION_SELECT_INTERFACE");
            pSelectInterface = (struct _URB_SELECT_INTERFACE*)pUrb;

            /* Check if there is interface information in the URB */
            if (pUrb->UrbHeader.Length > offsetof(struct _URB_SELECT_INTERFACE, Interface))
            {
                /* Calculate interfaces length */
                interfaces_len = pUrb->UrbHeader.Length;
                interfaces_len -= offsetof(struct _URB_SELECT_INTERFACE, Interface);

                KdPrint(("Header Len: %d Interfaces_len: %d\n",
                        pUrb->UrbHeader.Length, interfaces_len));

                USBPcapParseInterfaceInformation(pDeviceData,
                                                 &pSelectInterface->Interface,
                                                 interfaces_len);
            }
            break;
        }

        default:
            break;
    }

    if (USBPcapIsDeviceFiltered(&pDeviceData->pRootData->filter,
                                (int)pDeviceData->deviceAddress) == FALSE)
    {
        /* Do not log URBs from devices which are not being filtered */
        return;
    }

    if (hasUnknownURBSubmitInfo)
    {
        /* Simply log the unknown URB.
         *
         * Originally this was used to detect if URB returns from PDO as URB_FUNCTION_CONTROL_TRANSFER
         * When this was the case, the unknownURBSubmitInfo was used together with the pUrb to fake
         * the Setup stage packet. Unfortunately it is unreliable as there are specific Windows and
         * USB Root Hub combinations for which the URB can return with URB_FUNCTION_CONTROL_TRANSFER
         * but the SetupPacket inside pUrb does not contain valid data!
         */
        USBPCAP_BUFFER_PACKET_HEADER  packetHeader;

        DkDbgVal("Logging unknown URB from URB IRP table", unknownURBSubmitInfo.function);

        packetHeader.headerLen  = sizeof(USBPCAP_BUFFER_PACKET_HEADER);
        packetHeader.irpId      = (UINT64) pIrp;
        packetHeader.status     = unknownURBSubmitInfo.status;
        packetHeader.function   = unknownURBSubmitInfo.function;
        packetHeader.info       = unknownURBSubmitInfo.info;
        packetHeader.bus        = unknownURBSubmitInfo.bus;
        packetHeader.device     = unknownURBSubmitInfo.device;
        packetHeader.endpoint   = 0;
        packetHeader.transfer   = USBPCAP_TRANSFER_UNKNOWN;
        packetHeader.dataLength = 0;

        USBPcapBufferWriteTimestampedPacket(pDeviceData->pRootData,
                                            unknownURBSubmitInfo.timestamp,
                                            &packetHeader, NULL);
    }

    switch (header->Function)
    {
        case URB_FUNCTION_SELECT_CONFIGURATION:
        {
            struct _URB_SELECT_CONFIGURATION *pSelectConfiguration;
            struct _URB_CONTROL_TRANSFER     wrapTransfer;

            pSelectConfiguration = (struct _URB_SELECT_CONFIGURATION*)pUrb;

            wrapTransfer.PipeHandle = NULL; /* Default pipe handle */
            wrapTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_OUT;
            wrapTransfer.TransferBufferLength = 0;
            wrapTransfer.TransferBuffer = NULL;
            wrapTransfer.TransferBufferMDL = NULL;
            wrapTransfer.SetupPacket[0] = 0x00; /* Host to Device, Standard */
            wrapTransfer.SetupPacket[1] = 0x09; /* SET_CONFIGURATION */
            if (pSelectConfiguration->ConfigurationDescriptor == NULL)
            {
                wrapTransfer.SetupPacket[2] = 0;
            }
            else
            {
                wrapTransfer.SetupPacket[2] = pSelectConfiguration->ConfigurationDescriptor->bConfigurationValue;
            }
            wrapTransfer.SetupPacket[3] = 0;
            wrapTransfer.SetupPacket[4] = 0;
            wrapTransfer.SetupPacket[5] = 0;
            wrapTransfer.SetupPacket[6] = 0;
            wrapTransfer.SetupPacket[7] = 0;

            USBPcapAnalyzeControlTransfer(&wrapTransfer, header,
                                          pDeviceData, pIrp, post);
            break;
        }

        case URB_FUNCTION_SELECT_INTERFACE:
        {
            struct _URB_SELECT_INTERFACE *pSelectInterface;
            struct _URB_CONTROL_TRANSFER wrapTransfer;
            PUSBD_INTERFACE_INFORMATION  intInfo;
            PUSB_INTERFACE_DESCRIPTOR    intDescriptor;

            pSelectInterface = (struct _URB_SELECT_INTERFACE*)pUrb;

            if (pDeviceData->descriptor == NULL)
            {
                /* Won't log this URB */
                DkDbgStr("No configuration descriptor");
                break;
            }

            /* Obtain the USB_INTERFACE_DESCRIPTOR */
            intInfo = &pSelectInterface->Interface;

            intDescriptor =
                USBD_ParseConfigurationDescriptorEx(pDeviceData->descriptor,
                                                    pDeviceData->descriptor,
                                                    intInfo->InterfaceNumber,
                                                    intInfo->AlternateSetting,
                                                    -1,  /* Class */
                                                    -1,  /* SubClass */
                                                    -1); /* Protocol */

            if (intDescriptor == NULL)
            {
                /* Interface descriptor not found */
                DkDbgStr("Failed to get interface descriptor");
                break;
            }

            wrapTransfer.PipeHandle = NULL; /* Default pipe handle */
            wrapTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_OUT;
            wrapTransfer.TransferBufferLength = 0;
            wrapTransfer.TransferBuffer = NULL;
            wrapTransfer.TransferBufferMDL = NULL;
            wrapTransfer.SetupPacket[0] = 0x00; /* Host to Device, Standard */
            wrapTransfer.SetupPacket[1] = 0x0B; /* SET_INTERFACE */

            wrapTransfer.SetupPacket[2] = intDescriptor->bAlternateSetting;
            wrapTransfer.SetupPacket[3] = 0;
            wrapTransfer.SetupPacket[4] = intDescriptor->bInterfaceNumber;
            wrapTransfer.SetupPacket[5] = 0;
            wrapTransfer.SetupPacket[6] = 0;
            wrapTransfer.SetupPacket[7] = 0;

            USBPcapAnalyzeControlTransfer(&wrapTransfer, header,
                                          pDeviceData, pIrp, post);
            break;
        }

        case URB_FUNCTION_CONTROL_TRANSFER:
        {
            struct _URB_CONTROL_TRANSFER* transfer;

            transfer = (struct _URB_CONTROL_TRANSFER*)pUrb;

            DkDbgStr("URB_FUNCTION_CONTROL_TRANSFER");
            USBPcapAnalyzeControlTransfer(transfer, header,
                                          pDeviceData, pIrp, post);

            DkDbgVal("", transfer->PipeHandle);
            USBPcapPrintChars("Setup Packet", &transfer->SetupPacket[0], 8);
            if (transfer->TransferBuffer != NULL)
            {
                USBPcapPrintChars("Transfer Buffer",
                                 transfer->TransferBuffer,
                                 transfer->TransferBufferLength);
            }
            break;
        }

#if (_WIN32_WINNT >= 0x0600)
        case URB_FUNCTION_CONTROL_TRANSFER_EX:
        {
            struct _URB_CONTROL_TRANSFER     wrapTransfer;
            struct _URB_CONTROL_TRANSFER_EX* transfer;

            transfer = (struct _URB_CONTROL_TRANSFER_EX*)pUrb;

            DkDbgStr("URB_FUNCTION_CONTROL_TRANSFER_EX");

            /* Copy the required data to wrapTransfer */
            wrapTransfer.PipeHandle = transfer->PipeHandle;
            wrapTransfer.TransferFlags = transfer->TransferFlags;
            wrapTransfer.TransferBufferLength = transfer->TransferBufferLength;
            wrapTransfer.TransferBuffer = transfer->TransferBuffer;
            wrapTransfer.TransferBufferMDL = transfer->TransferBufferMDL;
            RtlCopyMemory(&wrapTransfer.SetupPacket[0],
                          &transfer->SetupPacket[0],
                          8 /* Setup packet is always 8 bytes */);

            USBPcapAnalyzeControlTransfer(&wrapTransfer, header,
                                          pDeviceData, pIrp, post);

            DkDbgVal("", transfer->PipeHandle);
            USBPcapPrintChars("Setup Packet", &transfer->SetupPacket[0], 8);
            if (transfer->TransferBuffer != NULL)
            {
                USBPcapPrintChars("Transfer Buffer",
                                  transfer->TransferBuffer,
                                  transfer->TransferBufferLength);
            }
            break;
        }
#endif

        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
        {
            struct _URB_CONTROL_TRANSFER             wrapTransfer;
            struct _URB_CONTROL_DESCRIPTOR_REQUEST*  request;

            request = (struct _URB_CONTROL_DESCRIPTOR_REQUEST*)pUrb;

            DkDbgVal("URB_FUNCTION_XXX_DESCRIPTOR", header->Function);

            /* Set up wrapTransfer */
            wrapTransfer.PipeHandle = NULL; /* Default pipe handle */
            if (header->Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE)
            {
                wrapTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_IN;
                /* D7: Data from Device to Host (1)
                 * D6-D5: Standard (0)
                 * D4-D0: Device (0)
                 */
                wrapTransfer.SetupPacket[0] = 0x80;
                /* 0x06 - GET_DESCRIPTOR */
                wrapTransfer.SetupPacket[1] = 0x06;
            }
            else if (header->Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT)
            {
                wrapTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_IN;
                /* D7: Data from Device to Host (1)
                 * D6-D5: Standard (0)
                 * D4-D0: Endpoint (2)
                 */
                wrapTransfer.SetupPacket[0] = 0x82;
                /* 0x06 - GET_DESCRIPTOR */
                wrapTransfer.SetupPacket[1] = 0x06;
            }
            else if (header->Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE)
            {
                wrapTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_IN;
                /* D7: Data from Device to Host (1)
                 * D6-D5: Standard (0)
                 * D4-D0: Interface (1)
                 */
                wrapTransfer.SetupPacket[0] = 0x81;
                /* 0x06 - GET_DESCRIPTOR */
                wrapTransfer.SetupPacket[1] = 0x06;
            }
            else if (header->Function == URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE)
            {
                wrapTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_OUT;
                /* D7: Data from Host to Device (0)
                 * D6-D5: Standard (0)
                 * D4-D0: Device (0)
                 */
                wrapTransfer.SetupPacket[0] = 0x00;
                /* 0x07 - SET_DESCRIPTOR */
                wrapTransfer.SetupPacket[1] = 0x07;
            }
            else if (header->Function == URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT)
            {
                wrapTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_OUT;
                /* D7: Data from Host to Device (0)
                 * D6-D5: Standard (0)
                 * D4-D0: Endpoint (2)
                 */
                wrapTransfer.SetupPacket[0] = 0x02;
                /* 0x07 - SET_DESCRIPTOR */
                wrapTransfer.SetupPacket[1] = 0x07;
            }
            else if (header->Function == URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE)
            {
                wrapTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_OUT;
                /* D7: Data from Host to Device (0)
                 * D6-D5: Standard (0)
                 * D4-D0: Interface (1)
                 */
                wrapTransfer.SetupPacket[0] = 0x01;
                /* 0x07 - SET_DESCRIPTOR */
                wrapTransfer.SetupPacket[1] = 0x07;
            }
            else
            {
                DkDbgVal("Invalid function", header->Function);
                break;
            }
            wrapTransfer.SetupPacket[2] = request->Index;
            wrapTransfer.SetupPacket[3] = request->DescriptorType;
            wrapTransfer.SetupPacket[4] = (request->LanguageId & 0x00FF);
            wrapTransfer.SetupPacket[5] = (request->LanguageId & 0xFF00) >> 8;
            wrapTransfer.SetupPacket[6] = (request->TransferBufferLength & 0x00FF);
            wrapTransfer.SetupPacket[7] = (request->TransferBufferLength & 0xFF00) >> 8;

            wrapTransfer.TransferBufferLength = request->TransferBufferLength;
            wrapTransfer.TransferBuffer = request->TransferBuffer;
            wrapTransfer.TransferBufferMDL = request->TransferBufferMDL;

            USBPcapAnalyzeControlTransfer(&wrapTransfer, header,
                                          pDeviceData, pIrp, post);
            break;
        }

        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
        case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
        case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
        case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        {
            struct _URB_CONTROL_TRANSFER             wrapTransfer;
            struct _URB_CONTROL_GET_STATUS_REQUEST*  request;

            request = (struct _URB_CONTROL_GET_STATUS_REQUEST*)pUrb;

            DkDbgVal("URB_FUNCTION_GET_STATUS_FROM_XXX", header->Function);

            /* Set up wrapTransfer */
            wrapTransfer.PipeHandle = NULL; /* Default pipe handle */
            wrapTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_IN;

            if (header->Function == URB_FUNCTION_GET_STATUS_FROM_DEVICE)
            {
                /* D7: Data from Device to Host (1)
                 * D6-D5: Standard (0)
                 * D4-D0: Device (0)
                 */
                wrapTransfer.SetupPacket[0] = 0x80;
            }
            else if (header->Function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE)
            {
                /* D7: Data from Device to Host (1)
                 * D6-D5: Standard (0)
                 * D4-D0: Interface (1)
                 */
                wrapTransfer.SetupPacket[0] = 0x81;
            }
            else if (header->Function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT)
            {
                /* D7: Data from Device to Host (1)
                 * D6-D5: Standard (0)
                 * D4-D0: Endpoint (2)
                 */
                wrapTransfer.SetupPacket[0] = 0x82;
            }
            else if (header->Function == URB_FUNCTION_GET_STATUS_FROM_OTHER)
            {
                /* D7: Data from Device to Host (1)
                 * D6-D5: Standard (0)
                 * D4-D0: Other (3)
                 */
                wrapTransfer.SetupPacket[0] = 0x83;
            }
            else
            {
                DkDbgVal("Invalid function", header->Function);
                break;
            }

            /* 0x00 - GET_STATUS */
            wrapTransfer.SetupPacket[1] = 0x00;
            /* wValue is Zero */
            wrapTransfer.SetupPacket[2] = 0;
            wrapTransfer.SetupPacket[3] = 0;
            /* wIndex */
            wrapTransfer.SetupPacket[4] = (request->Index & 0x00FF);
            wrapTransfer.SetupPacket[5] = (request->Index & 0xFF00) >> 8;
            /* wLength must be 2 */
            wrapTransfer.SetupPacket[6] = (request->TransferBufferLength & 0x00FF);
            wrapTransfer.SetupPacket[7] = (request->TransferBufferLength & 0xFF00) >> 8;

            wrapTransfer.TransferBufferLength = request->TransferBufferLength;
            wrapTransfer.TransferBuffer = request->TransferBuffer;
            wrapTransfer.TransferBufferMDL = request->TransferBufferMDL;

            USBPcapAnalyzeControlTransfer(&wrapTransfer, header,
                                          pDeviceData, pIrp, post);
            break;
        }

        case URB_FUNCTION_VENDOR_DEVICE:
        case URB_FUNCTION_VENDOR_INTERFACE:
        case URB_FUNCTION_VENDOR_ENDPOINT:
        case URB_FUNCTION_VENDOR_OTHER:
        case URB_FUNCTION_CLASS_DEVICE:
        case URB_FUNCTION_CLASS_INTERFACE:
        case URB_FUNCTION_CLASS_ENDPOINT:
        case URB_FUNCTION_CLASS_OTHER:
        {
            struct _URB_CONTROL_TRANSFER                  wrapTransfer;
            struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST*  request;

            request = (struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST*)pUrb;

            DkDbgVal("URB_FUNCTION_VENDOR_XXX/URB_FUNCTION_CLASS_XXX",
                     header->Function);

            wrapTransfer.PipeHandle = NULL; /* Default pipe handle */
            wrapTransfer.TransferFlags = request->TransferFlags;
            wrapTransfer.TransferBufferLength = request->TransferBufferLength;
            wrapTransfer.TransferBuffer = request->TransferBuffer;
            wrapTransfer.TransferBufferMDL = request->TransferBufferMDL;

            /* Set up D6-D0 of Request Type based on Function
             * D7 (Data Stage direction) will be set later
             */
            switch (header->Function)
            {
                case URB_FUNCTION_VENDOR_DEVICE:
                    /* D4-D0: Device (0)
                     * D6-D5: Vendor (2)
                     */
                    wrapTransfer.SetupPacket[0] = 0x40;
                    break;
                case URB_FUNCTION_VENDOR_INTERFACE:
                    /* D4-D0: Interface (1)
                     * D6-D5: Vendor (2)
                     */
                    wrapTransfer.SetupPacket[0] = 0x41;
                    break;
                case URB_FUNCTION_VENDOR_ENDPOINT:
                    /* D4-D0: Endpoint (2)
                     * D6-D5: Vendor (2)
                     */
                    wrapTransfer.SetupPacket[0] = 0x42;
                    break;
                case URB_FUNCTION_VENDOR_OTHER:
                    /* D4-D0: Other (3)
                     * D6-D5: Vendor (2)
                     */
                    wrapTransfer.SetupPacket[0] = 0x43;
                    break;
                case URB_FUNCTION_CLASS_DEVICE:
                    /* D4-D0: Device (0)
                     * D6-D5: Class (1)
                     */
                    wrapTransfer.SetupPacket[0] = 0x20;
                    break;
                case URB_FUNCTION_CLASS_INTERFACE:
                    /* D4-D0: Interface (1)
                     * D6-D5: Class (1)
                     */
                    wrapTransfer.SetupPacket[0] = 0x21;
                    break;
                case URB_FUNCTION_CLASS_ENDPOINT:
                    /* D4-D0: Endpoint (2)
                     * D6-D5: Class (1)
                     */
                    wrapTransfer.SetupPacket[0] = 0x22;
                    break;
                case URB_FUNCTION_CLASS_OTHER:
                    /* D4-D0: Other (3)
                     * D6-D5: Class (1)
                     */
                    wrapTransfer.SetupPacket[0] = 0x23;
                    break;
                default:
                    DkDbgVal("Invalid function", header->Function);
                    break;
            }

            if (request->TransferFlags & USBD_TRANSFER_DIRECTION_IN)
            {
                /* Set D7: Request data from device */
                wrapTransfer.SetupPacket[0] |= 0x80;
            }

            wrapTransfer.SetupPacket[1] = request->Request;
            wrapTransfer.SetupPacket[2] = (request->Value & 0x00FF);
            wrapTransfer.SetupPacket[3] = (request->Value & 0xFF00) >> 8;
            wrapTransfer.SetupPacket[4] = (request->Index & 0x00FF);
            wrapTransfer.SetupPacket[5] = (request->Index & 0xFF00) >> 8;
            wrapTransfer.SetupPacket[6] = (request->TransferBufferLength & 0x00FF);
            wrapTransfer.SetupPacket[7] = (request->TransferBufferLength & 0xFF00) >> 8;

            USBPcapAnalyzeControlTransfer(&wrapTransfer, header,
                                          pDeviceData, pIrp, post);
            break;
        }

        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        {
            struct _URB_BULK_OR_INTERRUPT_TRANSFER  *transfer;
            USBPCAP_ENDPOINT_INFO                   info;
            BOOLEAN                                 epFound;
            USBPCAP_BUFFER_PACKET_HEADER            packetHeader;
            PVOID                                   transferBuffer;

            packetHeader.headerLen = sizeof(USBPCAP_BUFFER_PACKET_HEADER);
            packetHeader.irpId     = (UINT64) pIrp;
            packetHeader.status    = header->Status;
            packetHeader.function  = header->Function;
            packetHeader.info      = 0;
            if (post == TRUE)
            {
                packetHeader.info |= USBPCAP_INFO_PDO_TO_FDO;
            }

            packetHeader.bus      = pDeviceData->pRootData->busId;

            transfer = (struct _URB_BULK_OR_INTERRUPT_TRANSFER*)pUrb;

            DkDbgStr("URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER");
            DkDbgVal("", transfer->PipeHandle);
            epFound = USBPcapRetrieveEndpointInfo(pDeviceData,
                                                  transfer->PipeHandle,
                                                  &info);
            if (epFound == TRUE)
            {
                packetHeader.device = info.deviceAddress;
                packetHeader.endpoint = info.endpointAddress;

                switch (info.type)
                {
                    case UsbdPipeTypeInterrupt:
                        packetHeader.transfer = USBPCAP_TRANSFER_INTERRUPT;
                        break;
                    default:
                        DkDbgVal("Invalid pipe type. Assuming bulk.",
                                 info.type);
                        /* Fall through */
                    case UsbdPipeTypeBulk:
                        packetHeader.transfer = USBPCAP_TRANSFER_BULK;
                        break;
                }
            }
            else
            {
                packetHeader.device = pDeviceData->deviceAddress;
                packetHeader.endpoint = 0xFF;
                packetHeader.transfer = USBPCAP_TRANSFER_BULK;
            }

            /* For IN endpoints, add data to log only when post = TRUE,
             * For OUT endpoints, add data to log only when post = FALSE
             */
            if (((packetHeader.endpoint & 0x80) && (post == TRUE)) ||
                (!(packetHeader.endpoint & 0x80) && (post == FALSE)))
            {
                packetHeader.dataLength = (UINT32)transfer->TransferBufferLength;

                transferBuffer =
                    USBPcapURBGetBufferPointer(transfer->TransferBufferLength,
                                               transfer->TransferBuffer,
                                               transfer->TransferBufferMDL);
            }
            else
            {
                packetHeader.dataLength = 0;
                transferBuffer = NULL;
            }

            USBPcapBufferWritePacket(pDeviceData->pRootData,
                                     &packetHeader,
                                     transferBuffer);

            DkDbgVal("", transfer->TransferFlags);
            DkDbgVal("", transfer->TransferBufferLength);
            DkDbgVal("", transfer->TransferBuffer);
            DkDbgVal("", transfer->TransferBufferMDL);
            if (transfer->TransferBuffer != NULL)
            {
                USBPcapPrintChars("Transfer Buffer",
                                  transfer->TransferBuffer,
                                  transfer->TransferBufferLength);
            }
            break;
        }

        case URB_FUNCTION_ISOCH_TRANSFER:
        {
            struct _URB_ISOCH_TRANSFER    *transfer;
            USBPCAP_ENDPOINT_INFO         info;
            BOOLEAN                       epFound;
            PUSBPCAP_BUFFER_ISOCH_HEADER  packetHeader;
            PUSBPCAP_PAYLOAD_ENTRY        compactedPayloadEntries;
            PVOID                         captureBuffer;
            USHORT                        headerLen;
            ULONG                         i;

            transfer = (struct _URB_ISOCH_TRANSFER*)pUrb;

            DkDbgStr("URB_FUNCTION_ISOCH_TRANSFER");
            DkDbgVal("", transfer->PipeHandle);
            DkDbgVal("", transfer->TransferFlags);
            DkDbgVal("", transfer->NumberOfPackets);

            /* Handle transfers up to maximum of 1024 packets */
            if (transfer->NumberOfPackets > 1024)
            {
                DkDbgVal("Too many packets for isochronous transfer",
                         transfer->NumberOfPackets);
                break;
            }

            /* headerLen will fit on 16 bits for every allowed value of
             * NumberOfPackets */
            headerLen = (USHORT)sizeof(USBPCAP_BUFFER_ISOCH_HEADER) +
                        (USHORT)(sizeof(USBPCAP_BUFFER_ISO_PACKET) *
                                 (transfer->NumberOfPackets - 1));

            packetHeader = ExAllocatePoolWithTag(NonPagedPool,
                                                 (SIZE_T)headerLen,
                                                 ' RDH');

            if (packetHeader == NULL)
            {
                DkDbgStr("Insufficient resources for isochronous transfer");
                break;
            }

            packetHeader->header.headerLen = headerLen;
            packetHeader->header.irpId     = (UINT64) pIrp;
            packetHeader->header.status    = header->Status;
            packetHeader->header.function  = header->Function;
            packetHeader->header.info      = 0;
            if (post == TRUE)
            {
                packetHeader->header.info |= USBPCAP_INFO_PDO_TO_FDO;
            }

            packetHeader->header.bus       = pDeviceData->pRootData->busId;

            epFound = USBPcapRetrieveEndpointInfo(pDeviceData,
                                                  transfer->PipeHandle,
                                                  &info);
            if (epFound == TRUE)
            {
                packetHeader->header.device = info.deviceAddress;
                packetHeader->header.endpoint = info.endpointAddress;
            }
            else
            {
                packetHeader->header.device = pDeviceData->deviceAddress;
                packetHeader->header.endpoint = 0xFF;
            }
            packetHeader->header.transfer = USBPCAP_TRANSFER_ISOCHRONOUS;

            /* Default to no data, will be changed later if data is to be attached to packet */
            packetHeader->header.dataLength = 0;
            compactedPayloadEntries = NULL;
            captureBuffer = NULL;

            /* Copy the packet headers untouched */
            for (i = 0; i < transfer->NumberOfPackets; i++)
            {
                packetHeader->packet[i].offset = transfer->IsoPacket[i].Offset;
                packetHeader->packet[i].length = transfer->IsoPacket[i].Length;
                packetHeader->packet[i].status = transfer->IsoPacket[i].Status;
            }

            /* For inbound isoch transfers (post), transfer->TransferBufferLength reflects the actual
             * number of bytes received. Rather than copying the entire transfer buffer (which may have
             * empty gaps), we will compact the data, copying only the packets that contain data.
             */
            if (transfer->TransferBufferLength != 0)
            {
                PUCHAR transferBuffer =
                        USBPcapURBGetBufferPointer(transfer->TransferBufferLength,
                                                   transfer->TransferBuffer,
                                                   transfer->TransferBufferMDL);

                if (((transfer->TransferFlags & USBD_TRANSFER_DIRECTION_IN) == USBD_TRANSFER_DIRECTION_IN) && (post == TRUE))
                {
                    ULONG  compactedOffset;
                    ULONG  compactedLength;

                    compactedLength = 0;

                    /* Compute the compacted transfer length by summing up the individual packet lengths */
                    for (i = 0; i < transfer->NumberOfPackets; i++)
                    {
                        compactedLength += transfer->IsoPacket[i].Length;
                    }

                    if (compactedLength > transfer->TransferBufferLength)
                    {
                        /* This is a safety check -- the numbers don't add up (this should never happen) */
                        DkDbgStr("Sum of Isochronous transfer packet lengths exceeds transfer buffer length");
                        ExFreePool((PVOID)packetHeader);
                        break;
                    }

                    /* Compact the data to minimize the capture size */
                    packetHeader->header.dataLength = (UINT32)compactedLength;

                    /* Allocate array of payload entries that will point to original data */
                    compactedPayloadEntries = ExAllocatePoolWithTag(NonPagedPool,
                        (SIZE_T)(transfer->NumberOfPackets + 1) * sizeof(USBPCAP_PAYLOAD_ENTRY),
                        'COSI');
                    if (compactedPayloadEntries == NULL)
                    {
                        DkDbgStr("Insufficient resources for isochronous transfer");
                        ExFreePool((PVOID)packetHeader);
                        break;
                    }

                    /* Loop through all the isoch packets in the transfer buffer
                     * Store offset and length in payload entries array in a way
                     * that there won't be gaps in the resulting packet.
                     */
                    compactedOffset = 0;
                    for (i = 0; i < transfer->NumberOfPackets; i++)
                    {
                        /* Adjust the offsets */
                        packetHeader->packet[i].offset = compactedOffset;
                        packetHeader->packet[i].length = transfer->IsoPacket[i].Length;
                        packetHeader->packet[i].status = transfer->IsoPacket[i].Status;

                        compactedPayloadEntries[i].size = transfer->IsoPacket[i].Length;
                        compactedPayloadEntries[i].buffer = &transferBuffer[transfer->IsoPacket[i].Offset];
                        compactedOffset += transfer->IsoPacket[i].Length;
                    }
                    compactedPayloadEntries[i].size = 0;
                    compactedPayloadEntries[i].buffer = NULL;
                }
                else if (((transfer->TransferFlags & USBD_TRANSFER_DIRECTION_IN) == USBD_TRANSFER_DIRECTION_OUT) && (post == FALSE))
                {
                    captureBuffer = transferBuffer;
                    packetHeader->header.dataLength = transfer->TransferBufferLength;
                }
                else
                {
                    /* Do not capture transfer buffer now */
                }
            }

            packetHeader->startFrame      = transfer->StartFrame;
            packetHeader->numberOfPackets = transfer->NumberOfPackets;
            packetHeader->errorCount      = transfer->ErrorCount;

            if (compactedPayloadEntries)
            {
                USBPcapBufferWritePayload(pDeviceData->pRootData,
                                          (PUSBPCAP_BUFFER_PACKET_HEADER)packetHeader,
                                          compactedPayloadEntries);
                ExFreePool((PVOID)compactedPayloadEntries);
            }
            else
            {
                USBPcapBufferWritePacket(pDeviceData->pRootData,
                                         (PUSBPCAP_BUFFER_PACKET_HEADER)packetHeader,
                                         captureBuffer);
            }

            ExFreePool((PVOID)packetHeader);
            break;
        }

        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        case URB_FUNCTION_SYNC_RESET_PIPE:
        case URB_FUNCTION_SYNC_CLEAR_STALL:
        case URB_FUNCTION_ABORT_PIPE:
#if (_WIN32_WINNT >= 0x0602)
        case URB_FUNCTION_CLOSE_STATIC_STREAMS:
#endif
        {
            struct _URB_PIPE_REQUEST      *request;
            USBPCAP_BUFFER_PACKET_HEADER   packetHeader;
            USBPCAP_ENDPOINT_INFO          info;
            BOOLEAN                        epFound;

            packetHeader.headerLen  = sizeof(USBPCAP_BUFFER_PACKET_HEADER);
            packetHeader.irpId      = (UINT64) pIrp;
            packetHeader.status     = header->Status;
            packetHeader.function   = header->Function;
            packetHeader.info       = 0;
            if (post == TRUE)
            {
                packetHeader.info |= USBPCAP_INFO_PDO_TO_FDO;
            }
            packetHeader.bus        = pDeviceData->pRootData->busId;
            packetHeader.transfer   = USBPCAP_TRANSFER_IRP_INFO;
            packetHeader.dataLength = 0;

            request = (struct _URB_PIPE_REQUEST*)pUrb;

            DkDbgVal("URB PIPE REQUEST", request->PipeHandle);
            epFound = USBPcapRetrieveEndpointInfo(pDeviceData,
                                                  request->PipeHandle,
                                                  &info);
            if (epFound == TRUE)
            {
                packetHeader.device = info.deviceAddress;
                packetHeader.endpoint = info.endpointAddress;
            }
            else
            {
                packetHeader.device = pDeviceData->deviceAddress;
                packetHeader.endpoint = 0xFF;
                packetHeader.transfer = USBPCAP_TRANSFER_UNKNOWN;
            }


            USBPcapBufferWritePacket(pDeviceData->pRootData,
                                     &packetHeader,
                                     NULL);
            break;
        }

        case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
        {
            struct _URB_GET_CURRENT_FRAME_NUMBER  *request;
            USBPCAP_BUFFER_PACKET_HEADER           packetHeader;
            UINT32                                 frameNum;

            request = (struct _URB_GET_CURRENT_FRAME_NUMBER*)pUrb;

            packetHeader.headerLen  = sizeof(USBPCAP_BUFFER_PACKET_HEADER);
            packetHeader.irpId      = (UINT64) pIrp;
            packetHeader.status     = header->Status;
            packetHeader.function   = header->Function;
            packetHeader.info       = 0;
            packetHeader.bus        = pDeviceData->pRootData->busId;
            packetHeader.device     = pDeviceData->deviceAddress;
            packetHeader.endpoint   = 0x80;
            packetHeader.transfer   = USBPCAP_TRANSFER_IRP_INFO;
            packetHeader.dataLength = 0;

            if (post == TRUE)
            {
                packetHeader.info |= USBPCAP_INFO_PDO_TO_FDO;
                frameNum = request->FrameNumber;
                packetHeader.dataLength = sizeof(frameNum);
            }

            USBPcapBufferWritePacket(pDeviceData->pRootData,
                                     &packetHeader,
                                     &frameNum);
            break;
        }

        default:
        {
            if (post == FALSE)
            {
                KIRQL irql;
                USBPCAP_URB_IRP_INFO info;

                /* Record unknown URB function to table.
                 * Some of the unknown URB change to control transfer on its way back
                 * from the PDO to FDO.
                 */
                DkDbgVal("Recording unknown URB type in URB IRP table", header->Function);

                info.irp = pIrp;
                info.timestamp = USBPcapGetCurrentTimestamp();
                info.status = header->Status;
                info.function = header->Function;
                info.info = 0;
                info.bus = pDeviceData->pRootData->busId;
                info.device = pDeviceData->deviceAddress;

                KeAcquireSpinLock(&pDeviceData->tablesSpinLock, &irql);
                USBPcapAddURBIRPInfo(pDeviceData->URBIrpTable, &info);
                KeReleaseSpinLock(&pDeviceData->tablesSpinLock, irql);
            }
            else /* if (post == TRUE) */
            {
                USBPCAP_BUFFER_PACKET_HEADER  packetHeader;

                DkDbgVal("Unknown URB type", header->Function);

                packetHeader.headerLen  = sizeof(USBPCAP_BUFFER_PACKET_HEADER);
                packetHeader.irpId      = (UINT64) pIrp;
                packetHeader.status     = header->Status;
                packetHeader.function   = header->Function;
                packetHeader.info       = USBPCAP_INFO_PDO_TO_FDO;

                packetHeader.bus        = pDeviceData->pRootData->busId;
                packetHeader.device     = pDeviceData->deviceAddress;
                packetHeader.endpoint   = 0;
                packetHeader.transfer   = USBPCAP_TRANSFER_UNKNOWN;
                packetHeader.dataLength = 0;

                USBPcapBufferWritePacket(pDeviceData->pRootData, &packetHeader, NULL);
            }
        }
    }
}
