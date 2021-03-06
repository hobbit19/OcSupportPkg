/** @file
  Copyright (C) 2019, vit9696. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "BootManagementInternal.h"

#include <Guid/AppleVariable.h>
#include <Guid/GlobalVariable.h>
#include <Guid/OcVariables.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcStringLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

/**
  Retrieves booting relevant data from an UEFI Boot#### option.
  If BootName is NULL, a BDS-style process is assumed and inactive as well as
  non-Boot type applications are ignored.

  @param[in]  BootOption        The boot option's index.
  @param[out] BootName          On output, the boot option's description.
  @param[out] OptionalDataSize  On output, the optional data size.
  @param[out] OptionalData      On output, a pointer to the optional data.

**/
STATIC
EFI_DEVICE_PATH_PROTOCOL *
InternalGetBootOptionData (
  IN  UINT16   BootOption,
  IN  EFI_GUID *BootGud,
  OUT CHAR16   **BootName  OPTIONAL,
  OUT UINT32   *OptionalDataSize  OPTIONAL,
  OUT VOID     **OptionalData  OPTIONAL
  )
{
  EFI_STATUS               Status;
  CHAR16                   BootVarName[L_STR_LEN (L"Boot####") + 1];

  UINTN                    LoadOptionSize;
  EFI_LOAD_OPTION          *LoadOption;
  UINT8                    *LoadOptionPtr;

  UINT32                   Attributes;
  CONST CHAR16             *Description;
  UINTN                    DescriptionSize;
  UINT16                   FilePathListSize;
  EFI_DEVICE_PATH_PROTOCOL *FilePathList;

  CHAR16                   *BootOptionName;
  VOID                     *OptionalDataBuffer;

  UnicodeSPrint (BootVarName, sizeof (BootVarName), L"Boot%04x", BootOption);

  Status = GetVariable2 (
             BootVarName,
             BootGud,
             (VOID **)&LoadOption,
             &LoadOptionSize
             );
  if (EFI_ERROR (Status) || (LoadOptionSize < sizeof (*LoadOption))) {
    return NULL;
  }

  Attributes = LoadOption->Attributes;
  if ((BootName == NULL)
   && (((Attributes & LOAD_OPTION_ACTIVE) == 0)
    || ((Attributes & LOAD_OPTION_CATEGORY) != LOAD_OPTION_CATEGORY_BOOT))) {
    FreePool (LoadOption);
    return NULL;
  }

  FilePathListSize = LoadOption->FilePathListLength;

  LoadOptionPtr   = (UINT8 *)(LoadOption + 1);
  LoadOptionSize -= sizeof (*LoadOption);

  if (FilePathListSize > LoadOptionSize) {
    FreePool (LoadOption);
    return NULL;
  }

  LoadOptionSize -= FilePathListSize;

  Description     = (CHAR16 *)LoadOptionPtr;
  DescriptionSize = StrnSizeS (Description, (LoadOptionSize / sizeof (CHAR16)));
  if (DescriptionSize > LoadOptionSize) {
    FreePool (LoadOption);
    return NULL;
  }

  LoadOptionPtr  += DescriptionSize;
  LoadOptionSize -= DescriptionSize;

  FilePathList = (EFI_DEVICE_PATH_PROTOCOL *)LoadOptionPtr;
  if (!IsDevicePathValid (FilePathList, FilePathListSize)) {
    FreePool (LoadOption);
    return NULL;
  }

  LoadOptionPtr += FilePathListSize;

  BootOptionName = NULL;

  if (BootName != NULL) {
    BootOptionName = AllocateCopyPool (DescriptionSize, Description);
  }

  OptionalDataBuffer = NULL;

  if (OptionalDataSize != NULL) {
    ASSERT (OptionalData != NULL);
    if (LoadOptionSize > 0) {
      OptionalDataBuffer = AllocateCopyPool (LoadOptionSize, LoadOptionPtr);
      if (OptionalDataBuffer == NULL) {
        LoadOptionSize = 0;
      }
    }

    *OptionalDataSize = (UINT32)LoadOptionSize;
  }
  //
  // Use the allocated Load Option buffer for the Device Path.
  //
  CopyMem (LoadOption, FilePathList, FilePathListSize);
  FilePathList = (EFI_DEVICE_PATH_PROTOCOL *)LoadOption;

  if (BootName != NULL) {
    *BootName = BootOptionName;
  }

  if (OptionalData != NULL) {
    *OptionalData = OptionalDataBuffer;
  }

  return FilePathList;
}

STATIC
VOID
InternalDebugBootEnvironment (
  IN CONST UINT16             *BootOrder,
  IN EFI_GUID                 *BootGuid,
  IN UINTN                    BootOrderSize
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *UefiDevicePath;
  UINTN                     UefiDevicePathSize;
  CHAR16                    *DevicePathText;
  UINTN                     Index;
  INT32                     Predefined;

  STATIC CONST CHAR16 *AppleDebugVariables[] = {
    L"efi-boot-device-data",
    L"efi-backup-boot-device-data",
    L"efi-apple-recovery-data"
  };

  STATIC CONST UINT16  ApplePredefinedVariables[] = {
    0x80, 0x81, 0x82
  };

  for (Index = 0; Index < ARRAY_SIZE (AppleDebugVariables); ++Index) {
    Status = GetVariable2 (
               AppleDebugVariables[Index],
               &gAppleBootVariableGuid,
               (VOID **)&UefiDevicePath,
               &UefiDevicePathSize
               );
    if (!EFI_ERROR (Status) && IsDevicePathValid (UefiDevicePath, UefiDevicePathSize)) {
      DevicePathText = ConvertDevicePathToText (UefiDevicePath, FALSE, FALSE);
      if (DevicePathText != NULL) {
        DEBUG ((DEBUG_INFO, "OCB: %s = %s\n", AppleDebugVariables[Index], DevicePathText));
        FreePool (DevicePathText);
        FreePool (UefiDevicePath);
        continue;
      }

      FreePool (UefiDevicePath);
    }
    DEBUG ((DEBUG_INFO, "OCB: %s - %r\n", AppleDebugVariables[Index], Status));
  }

  DEBUG ((DEBUG_INFO, "OCB: Dumping BootOrder\n"));
  
  for (Predefined = 0; Predefined < 2; ++Predefined) {
    for (Index = 0; Index < (BootOrderSize / sizeof (*BootOrder)); ++Index) {
      UefiDevicePath = InternalGetBootOptionData (
                         BootOrder[Index],
                         BootGuid,
                         NULL,
                         NULL,
                         NULL
                         );
      if (UefiDevicePath == NULL) {
        DEBUG ((
          DEBUG_INFO,
          "OCB: %u -> Boot%04x - failed to read\n",
          (UINT32) Index,
          BootOrder[Index]
          ));
        continue;
      }

      DevicePathText = ConvertDevicePathToText (UefiDevicePath, FALSE, FALSE);
      DEBUG ((
        DEBUG_INFO,
        "OCB: %u -> Boot%04x = %s\n",
        (UINT32) Index,
        BootOrder[Index],
        DevicePathText
        ));
      if (DevicePathText != NULL) {
        FreePool (DevicePathText);
      }

      FreePool (UefiDevicePath);
    }

    //
    // Redo with predefined.
    //
    BootOrder     = &ApplePredefinedVariables[0];
    BootOrderSize = sizeof (ApplePredefinedVariables);
    DEBUG ((DEBUG_INFO, "OCB: Predefined list\n"));
  }
}

OC_BOOT_ENTRY *
InternalGetDefaultBootEntry (
  IN OUT OC_BOOT_ENTRY  *BootEntries,
  IN     UINTN          NumBootEntries,
  IN     BOOLEAN        CustomBootGuid,
  IN     EFI_HANDLE     LoadHandle  OPTIONAL
  )
{
  EFI_STATUS               Status;
  BOOLEAN                  Result;
  INTN                     CmpResult;

  UINT32                   BootNextAttributes;
  UINTN                    BootNextSize;
  BOOLEAN                  IsBootNext;

  UINT16                   *BootOrder;
  UINTN                    BootOrderSize;

  UINTN                    RootDevicePathSize;
  EFI_DEVICE_PATH_PROTOCOL *UefiDevicePath;
  EFI_DEVICE_PATH_PROTOCOL *UefiRemainingDevicePath;
  EFI_DEVICE_PATH_PROTOCOL *OcDevicePath;
  EFI_DEVICE_PATH_PROTOCOL *OcRemainingDevicePath;
  UINT32                   OptionalDataSize;
  VOID                     *OptionalData;
  CHAR16                   *DevicePathText1;
  CHAR16                   *DevicePathText2;
  EFI_GUID                 *BootVariableGuid;

  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  EFI_HANDLE               DeviceHandle;

  UINT16                   BootNextOptionIndex;

  OC_BOOT_ENTRY            *BootEntry;
  UINTN                    Index;

  ASSERT (BootEntries != NULL);
  ASSERT (NumBootEntries > 0);

  IsBootNext   = FALSE;
  OptionalData = NULL;

  if (CustomBootGuid) {
    BootVariableGuid = &gOcVendorVariableGuid;
  } else {
    BootVariableGuid = &gEfiGlobalVariableGuid;
  }

  BootNextSize = sizeof (BootNextOptionIndex);
  Status = gRT->GetVariable (
                  EFI_BOOT_NEXT_VARIABLE_NAME,
                  BootVariableGuid,
                  &BootNextAttributes,
                  &BootNextSize,
                  &BootNextOptionIndex
                  );
  if (Status == EFI_NOT_FOUND) {
    DEBUG ((DEBUG_INFO, "OCB: BootNext has not been found\n"));

    Status = GetVariable2 (
               EFI_BOOT_ORDER_VARIABLE_NAME,
               BootVariableGuid,
               (VOID **)&BootOrder,
               &BootOrderSize
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "OCB: BootOrder is unavailable - %r\n", Status));
      return NULL;
    }

    if (BootOrderSize < sizeof (*BootOrder)) {
      DEBUG ((DEBUG_WARN, "OCB: BootOrder is malformed - %x\n", (UINT32) BootOrderSize));
      FreePool (BootOrder);
      return NULL;
    }

    DEBUG_CODE_BEGIN ();
    InternalDebugBootEnvironment (BootOrder, BootVariableGuid, BootOrderSize);
    DEBUG_CODE_END ();

    UefiDevicePath = InternalGetBootOptionData (
                       BootOrder[0],
                       BootVariableGuid,
                       NULL,
                       NULL,
                       NULL
                       );
    if (UefiDevicePath == NULL) {
      FreePool (BootOrder);
      return NULL;
    }

    DevicePath = UefiDevicePath;
    Status = gBS->LocateDevicePath (
                    &gEfiSimpleFileSystemProtocolGuid,
                    &DevicePath,
                    &DeviceHandle
                    );
    if (!EFI_ERROR (Status) && (DeviceHandle == LoadHandle)) {
      DEBUG ((DEBUG_INFO, "OCB: Skipping OC bootstrap application\n"));
      //
      // Skip BOOTx64.EFI at BootOrder[0].
      //
      FreePool (UefiDevicePath);

      if (BootOrderSize < (2 * sizeof (*BootOrder))) {
        FreePool (BootOrder);
        return NULL;
      }

      UefiDevicePath = InternalGetBootOptionData (
                         BootOrder[1],
                         BootVariableGuid,
                         NULL,
                         NULL,
                         NULL
                         );
      if (UefiDevicePath == NULL) {
        FreePool (BootOrder);
        return NULL;
      }
    }

    FreePool (BootOrder);
  } else if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCB: BootNext: %x\n", BootNextOptionIndex));
    //
    // BootNext must be deleted before attempting to start the image - delete
    // it here because not attempting to boot the image implies user's choice.
    //
    gRT->SetVariable (
           EFI_BOOT_NEXT_VARIABLE_NAME,
           BootVariableGuid,
           BootNextAttributes,
           0,
           NULL
           );
    IsBootNext = TRUE;

    UefiDevicePath = InternalGetBootOptionData (
                       BootNextOptionIndex,
                       BootVariableGuid,
                       NULL,
                       &OptionalDataSize,
                       &OptionalData
                       );
    if (UefiDevicePath == NULL) {
      return NULL;
    }
  } else {
    return NULL;
  }

  UefiRemainingDevicePath = UefiDevicePath;
  Result = OcFixAppleBootDevicePath (&UefiRemainingDevicePath);

  DEBUG_CODE_BEGIN ();
  DevicePathText1 = ConvertDevicePathToText (
                      UefiDevicePath,
                      FALSE,
                      FALSE
                      );
  DevicePathText2 = ConvertDevicePathToText (
                      UefiRemainingDevicePath,
                      FALSE,
                      FALSE
                      );
  
  DEBUG ((
    DEBUG_INFO,
    "OCB: Default boot device path: %s | remainder: %s | %s\n",
    DevicePathText1,
    DevicePathText2,
    (Result ? L"success" : L"failure")
    ));

  if (DevicePathText1 != NULL) {
    FreePool (DevicePathText1);
  }

  if (DevicePathText2 != NULL) {
    FreePool (DevicePathText2);
  }
  DEBUG_CODE_END ();

  if (!Result) {
    return NULL;
  }

  RootDevicePathSize = ((UINT8 *)UefiRemainingDevicePath - (UINT8 *)UefiDevicePath);

  for (Index = 0; Index < NumBootEntries; ++Index) {
    BootEntry    = &BootEntries[Index];
    OcDevicePath = BootEntry->DevicePath;

    if ((GetDevicePathSize (OcDevicePath) - END_DEVICE_PATH_LENGTH) < RootDevicePathSize) {
      continue;
    }

    CmpResult = CompareMem (OcDevicePath, UefiDevicePath, RootDevicePathSize);
    if (CmpResult != 0) {
      continue;
    }
    //
    // FIXME: Ensure that all the entries get properly filtered against any
    // malicious sources. The drive itself should already be safe, but it is
    // unclear whether a potentially safe device path can be transformed into
    // an unsafe one.
    //
    OcRemainingDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)(
                              (UINT8 *)OcDevicePath + RootDevicePathSize
                              );
    if (!IsBootNext) {
      //
      // For non-BootNext boot, the File Paths must match for the entries to be
      // matched. Startup Disk however only stores the drive's Device Path
      // excluding the booter path, which we treat as a match as well.
      //
      if (!IsDevicePathEnd (UefiRemainingDevicePath)
       && !IsDevicePathEqual (UefiRemainingDevicePath, OcRemainingDevicePath)
        ) {
        continue;
      }

      FreePool (UefiDevicePath);
    } else {
      //
      // BootNext is allowed to override both the exact file path as well as
      // the used load options.
      // TODO: Investigate whether Apple uses OptionalData, and exploit ways.
      //
      BootEntry->LoadOptionsSize = OptionalDataSize;
      BootEntry->LoadOptions     = OptionalData;
      //
      // Only use the BootNext path when it has a file path.
      //
      if (!IsDevicePathEnd (UefiRemainingDevicePath)) {
        //
        // TODO: Investigate whether macOS adds BootNext entries that are not
        //       possibly located by bless.
        //
        FreePool (BootEntry->DevicePath);
        BootEntry->DevicePath = UefiDevicePath;
      } else {
        FreePool (UefiDevicePath);
      }
    }

    DEBUG ((DEBUG_INFO, "OCB: Matched default boot option: %s\n", BootEntry->Name));

    return BootEntry;
  }

  if (OptionalData != NULL) {
    FreePool (OptionalData);
  }

  FreePool (UefiDevicePath);

  DEBUG ((DEBUG_WARN, "OCB: Failed to match a default boot option\n"));

  return NULL;
}
