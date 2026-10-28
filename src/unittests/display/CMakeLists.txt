# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
add_executable(DisplayFoundationTests DisplayFoundationTests.cpp)
target_link_libraries(DisplayFoundationTests PRIVATE deskflow-display)
foreach(case GoldenHeader Fragmented Coalesced MalformedHeaders Truncated PayloadLimits
             VideoBoundary ChannelIsolation ReplayIsolation Cancellation Recovery LeaseOwnership LeaseExpiry)
  add_test(NAME Display.${case} COMMAND DisplayFoundationTests ${case})
endforeach()
set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayFoundationTests)

# Protocol fixtures use only the Python standard library and the OpenSSL CLI.
find_package(Python3 QUIET COMPONENTS Interpreter)
find_program(DISPLAY_OPENSSL_EXECUTABLE openssl)
if(Python3_Interpreter_FOUND AND DISPLAY_OPENSSL_EXECUTABLE)
  get_filename_component(DISPLAY_PROTOTYPE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype" ABSOLUTE)
  add_test(NAME Display.Transport COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_display_transport)
  set_tests_properties(Display.Transport PROPERTIES
    WORKING_DIRECTORY "${DISPLAY_PROTOTYPE_DIR}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 30)

  add_test(NAME Display.CapabilityProbe COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_product_probe)
  set_tests_properties(Display.CapabilityProbe PROPERTIES WORKING_DIRECTORY "${DISPLAY_PROTOTYPE_DIR}" ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 30)

  option(DISPLAY_LIVE_TESTS "Run loopback TLS/FFmpeg display fixtures (opens sockets)" OFF)
  if(DISPLAY_LIVE_TESTS)
    find_program(DISPLAY_FFMPEG_EXECUTABLE ffmpeg REQUIRED)
    add_test(NAME Display.LiveSynthetic COMMAND "${Python3_EXECUTABLE}" validate_live_session.py)
    set_tests_properties(Display.LiveSynthetic PROPERTIES
      WORKING_DIRECTORY "${DISPLAY_PROTOTYPE_DIR}"
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 60 RUN_SERIAL TRUE)
  endif()
elseif(DISPLAY_LIVE_TESTS)
  message(FATAL_ERROR "DISPLAY_LIVE_TESTS requires Python3 and the OpenSSL CLI")
endif()

add_executable(DisplayCoordinatorTests CoordinatorTests.cpp)
target_link_libraries(DisplayCoordinatorTests PRIVATE deskflow-display)
foreach(case ConsentBeforeResources ExpiryAndLateConsent PermissionWaitPaused ReceiverPermissionBarrier PresentationGate PresentationRace
             CancelGeneration CleanupRequired BusyAndPreference InputConflict IdentityAndCapability LockDuringSession NoInputOrTelemetry DirectionalCapabilities)
  add_test(NAME Display.Product.${case} COMMAND DisplayCoordinatorTests ${case})
endforeach()
set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayCoordinatorTests)

if(TARGET deskflow-display-service AND TARGET Qt6::Test)
  add_executable(DisplayServiceTests DisplayServiceTests.cpp)
  target_link_libraries(DisplayServiceTests PRIVATE deskflow-display-service Qt6::Test)
  foreach(case AsyncAndSnapshot PeerAndRouteGates NoForgedCallbacks CancelQueuedPreparation PreferencePersistence JournalOwnership CorruptJournal ApplicationSingleton LayoutOwnership BluetoothPolicyGatesLowerService MeasuredStatisticsOwnership UnknownHostResourceRecovery)
    add_test(NAME Display.Service.${case} COMMAND DisplayServiceTests ${case})
    set_tests_properties(Display.Service.${case} PROPERTIES ENVIRONMENT
      "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profiles/DisplayServiceTests/config;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profiles/DisplayServiceTests/state")
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayServiceTests)
endif()

add_executable(UsageCoordinatorTests UsageCoordinatorTests.cpp)
target_link_libraries(UsageCoordinatorTests PRIVATE deskflow-display)
foreach(case IdleSelection KeepInputBeforeConsent BluetoothPreflightFailure ReleaseBeforeDisplay ReleaseFailureBlocks
             RollbackOnlyAfterCleanup NoRollbackWhenRevoked DisplayBeforeInput CleanupBlocksInput StopCancelsRollback
             CompleteInputPermission UnknownCapabilities StopDoesNotChangeMode CancelDuringRelease FocusReleaseBarrier CancelDisplayTeardown ConnectionLostDuringInputStart CancelRollback ReleaseTimeout OperationReplay InputStartTimeout)
  add_test(NAME Display.Usage.${case} COMMAND UsageCoordinatorTests ${case})
endforeach()
set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests UsageCoordinatorTests)

if(TARGET deskflow-display-service AND TARGET Qt6::Test)
  add_executable(PeerUsageServiceTests PeerUsageServiceTests.cpp)
  target_link_libraries(PeerUsageServiceTests PRIVATE deskflow-display-service Qt6::Test)
  foreach(case DirectionalReadiness ReceiverReadinessBlocked BluetoothOnlyBlocksExtended BluetoothPolicyRevokedDuringConsent BluetoothPolicyRevokedDuringDisplay SelectionAndStatus RejectSplitInput NoForgedCompletion SingleDisplayReservation CancelQueuedStart StalePermission DisplayTransactionBridge PeerRefusalPreservesInput InputRouteAdapterAcknowledgements InputRouteAdapterStaleResult ExistingConnectionSnapshot RejectStaleConnectionCommand UnifiedInputCleanup IncomingDisplayRetainsOriginalDirection RecoveryUsesUnifiedCleanup RecoveryAdoptsPendingConnection NormalDisconnectRetiresImportedInput PendingReconnectRetiresOnlyMatchingConnection SelectionPreferenceDoesNotStartUse UnavailableSessionDisablesRollback PeerCancelPreservesUnreleasedInput)
    add_test(NAME Display.UsageService.${case} COMMAND PeerUsageServiceTests ${case})
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests PeerUsageServiceTests)
endif()


# Manual only: creates actual virtual displays and visible native windows.
if(TARGET deskflow-display-service)
  add_executable(ProductRuntimeSmoke ProductRuntimeSmoke.cpp)
  target_link_libraries(ProductRuntimeSmoke PRIVATE deskflow-display-service Qt6::Core Qt6::Network)
endif()

if(TARGET deskflow-display-service AND TARGET Qt6::Test)
  add_executable(ScreenInventoryTests ScreenInventoryTests.cpp)
  target_link_libraries(ScreenInventoryTests PRIVATE deskflow-display-service Qt6::Test)
  add_test(NAME Display.ScreenInventory COMMAND ScreenInventoryTests)
endif()

add_executable(DisplayInfrastructurePlannerTests InfrastructurePlannerTests.cpp)
target_link_libraries(DisplayInfrastructurePlannerTests PRIVATE deskflow-display-infra-core)
foreach(case DirectionMatrix AsymmetricProtocols SafetyGates NoProtocolConversion
             CertificationInvalidation InvalidDeclarations FeedbackAndConditional PlanBinding CounterBounds)
  add_test(NAME Display.Infrastructure.${case} COMMAND DisplayInfrastructurePlannerTests ${case})
endforeach()
set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayInfrastructurePlannerTests)

add_executable(DisplayInfrastructureSessionTests InfrastructureSessionTests.cpp)
target_link_libraries(DisplayInfrastructureSessionTests PRIVATE deskflow-display-infra-core)
foreach(case AuthorizationDurability PresentationEvidence CancelDuringPersistence CancelDuringSpawn
             StaleFramesCleanup AbsoluteDeadlines RecoveryBudget GrantRevocationCleanup QueuedCreateCancelled)
  add_test(NAME Display.Infrastructure.${case} COMMAND DisplayInfrastructureSessionTests ${case})
endforeach()
set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayInfrastructureSessionTests)

if(TARGET deskflow-display-service)
  add_executable(DisplayInfrastructureControlTests InfrastructureControlTests.cpp)
  target_link_libraries(DisplayInfrastructureControlTests PRIVATE deskflow-display-infra-runtime)
  foreach(case StrictJson BindingReplay EnvelopeGates PlanCanonicalization)
    add_test(NAME Display.Infrastructure.${case} COMMAND DisplayInfrastructureControlTests ${case})
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayInfrastructureControlTests)
endif()

if(TARGET deskflow-display-service)
  add_executable(DurableResourceLedgerTests DurableResourceLedgerTests.cpp)
  target_link_libraries(DurableResourceLedgerTests PRIVATE deskflow-display-infra-runtime)
  foreach(case DurableIntentReplay ExclusiveLedger TornLedgerTail LedgerCorruption MissingCurrent
               LedgerCompaction DependencyOrder LedgerSymlink)
    add_test(NAME Display.Infrastructure.${case} COMMAND DurableResourceLedgerTests ${case})
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DurableResourceLedgerTests)
endif()

if(TARGET deskflow-display-infra-runtime)
  add_executable(DisplayWorkerProtocolTests WorkerProtocolTests.cpp)
  target_link_libraries(DisplayWorkerProtocolTests PRIVATE deskflow-display-infra-runtime)
  foreach(case WorkerHandshake WorkerBinding WorkerFraming WorkerState WorkerReplay WorkerDeadlines WorkerQueue WorkerPreparationContract WorkerSourceBinding WorkerSignalBudgets WorkerSignalDirection WorkerPreparationDeadline WorkerPlatformContract WorkerPurposeIsolation)
    add_test(NAME Display.Infrastructure.${case} COMMAND DisplayWorkerProtocolTests ${case})
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayWorkerProtocolTests)
endif()

if(TARGET deskflow-display-infra-runtime)
  add_executable(DisplayWorkerProcessTests WorkerProcessTests.cpp)
  target_link_libraries(DisplayWorkerProcessTests PRIVATE deskflow-display-infra-runtime)
  foreach(case WorkerProcessLifecycle WorkerRegistration WorkerExecutableBinding WorkerProcessFailures WorkerStopDeadline WorkerLegacyBootstrap WorkerParentEof WorkerFinalDrain)
    add_test(NAME Display.Infrastructure.${case} COMMAND DisplayWorkerProcessTests ${case})
    set_tests_properties(Display.Infrastructure.${case} PROPERTIES TIMEOUT 15)
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayWorkerProcessTests)
endif()

add_executable(DisplayFrameModelTests FrameModelTests.cpp)
target_link_libraries(DisplayFrameModelTests PRIVATE deskflow-display-infra-core)
foreach(case FrameLayouts FrameBudget FrameFreshness FrameTickets FrameFences FrameRetirement FrameQuarantine FrameDeleter)
  add_test(NAME Display.Infrastructure.${case} COMMAND DisplayFrameModelTests ${case})
  set_tests_properties(Display.Infrastructure.${case} PROPERTIES TIMEOUT 10)
endforeach()
set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayFrameModelTests)

if(TARGET deskflow-display-infra-runtime)
  add_executable(DisplayResourceExecutorTests ResourceExecutorTests.cpp)
  target_link_libraries(DisplayResourceExecutorTests PRIVATE deskflow-display-infra-runtime)
  foreach(case ResourceExecutionDurability ResourceCancelledCreation ResourceAmbiguousOwner ResourceIndependentAbsence
               ResourceDependencyCleanup ResourceIntentRecovery ResourceExecutionBarriers ResourceObservationFailure ResourceConfigurationBarrier ResourceLegacyRecord ResourceUnsupportedRecord ResourceProviderException)
    add_test(NAME Display.Infrastructure.${case} COMMAND DisplayResourceExecutorTests ${case})
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayResourceExecutorTests)
endif()

if(TARGET deskflow-display-infra-runtime)
  add_executable(DisplayControlAdmissionTests ControlAdmissionTests.cpp)
  target_link_libraries(DisplayControlAdmissionTests PRIVATE deskflow-display-infra-runtime)
  foreach(case ControlRateReservations CommandDeduplication CommandAbsoluteDeadline CommandGlobalBudget CommandCacheExpiry CommandSemanticDigest)
    add_test(NAME Display.Infrastructure.${case} COMMAND DisplayControlAdmissionTests ${case})
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayControlAdmissionTests)
endif()

# Manual only: creates a real Mac output; requires an unlocked console desktop.
if(TARGET deskflow-display-platform-macos)
  add_executable(MacPlatformHelperSmoke MacPlatformHelperSmoke.cpp)
  target_link_libraries(MacPlatformHelperSmoke PRIVATE deskflow-display-platform-macos)
endif()

if(TARGET deskflow-display-infra-runtime)
  add_executable(DisplayNativeProcessIdentityTests NativeProcessIdentityTests.cpp)
  target_link_libraries(DisplayNativeProcessIdentityTests PRIVATE deskflow-display-infra-runtime)
  foreach(case ProcessBirthIdentity ProcessBirthExit)
    add_test(NAME Display.Infrastructure.${case} COMMAND DisplayNativeProcessIdentityTests ${case})
    set_tests_properties(Display.Infrastructure.${case} PROPERTIES TIMEOUT 10)
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ${CMAKE_PROJECT_NAME}_tests DisplayNativeProcessIdentityTests)
endif()

if(TARGET deskflow-display-platform-macos)
  add_executable(MacResourceProviderSmoke MacResourceProviderSmoke.cpp)
  target_link_libraries(MacResourceProviderSmoke PRIVATE deskflow-display-platform-macos Qt6::Gui)
endif()
