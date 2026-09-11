### 2. Bound a resync envelope around archive, acks and pending commands
`RTEA.vcxproj`
- L852: `    <ClInclude Include="Source\Network\NetResyncState.h" />`
- L853: `    <ClInclude Include="Source\Network\NetResyncSelfTest.h" />`
- L1435: `    <ClCompile Include="Source\Network\NetResyncState.cpp" />`
- L1436: `    <ClCompile Include="Source\Network\NetResyncSelfTest.cpp" />`
`RTEA.vcxproj.filters`
- L535: `    <ClInclude Include="Source\Network\NetResyncState.h">`
- L536: `      <Filter>Network</Filter>`
- L537: `    </ClInclude>`
- L538: `    <ClInclude Include="Source\Network\NetResyncSelfTest.h">`
- L539: `      <Filter>Network</Filter>`
- L540: `    </ClInclude>`
- L892: `    <ClCompile Include="Source\Network\NetResyncState.cpp">`
- L893: `      <Filter>Network</Filter>`
- L894: `    </ClCompile>`
- L895: `    <ClCompile Include="Source\Network\NetResyncSelfTest.cpp">`
- L896: `      <Filter>Network</Filter>`
- L897: `    </ClCompile>`
`Source/Network/meson.build`
- L12: `'NetResyncState.cpp',`
- L13: `'NetResyncSelfTest.cpp',`

### 3. Restore complete input, drop-map ownership and exact-target priming
`RTEA.vcxproj`
- L854: `    <ClInclude Include="Source\Network\NetResyncRuntimeSelfTest.h" />`
- L1437: `    <ClCompile Include="Source\Network\NetResyncRuntimeSelfTest.cpp" />`
`RTEA.vcxproj.filters`
- L541: `    <ClInclude Include="Source\Network\NetResyncRuntimeSelfTest.h">`
- L542: `      <Filter>Network</Filter>`
- L543: `    </ClInclude>`
- L898: `    <ClCompile Include="Source\Network\NetResyncRuntimeSelfTest.cpp">`
- L899: `      <Filter>Network</Filter>`
- L900: `    </ClCompile>`
`Source/Network/meson.build`
- L14: `'NetResyncRuntimeSelfTest.cpp',`

### 7. Add A7 journal helpers for recovery-arm traces
`RTEA.vcxproj`
- L879: `    <ClInclude Include="Source\Network\NetA7Journal.h" />`
- L1461: `    <ClCompile Include="Source\Network\NetA7Journal.cpp" />`
`RTEA.vcxproj.filters`
- L460: `    <ClInclude Include="Source\Network\NetA7Journal.h">`
- L461: `      <Filter>Network</Filter>`
- L462: `    </ClInclude>`
- L817: `    <ClCompile Include="Source\Network\NetA7Journal.cpp">`
- L818: `      <Filter>Network</Filter>`
- L819: `    </ClCompile>`
`Source/Network/meson.build`
- L7: `'NetA7Journal.cpp',`

### 8. Give the host a live F6 panel and codec-17 seat snapshots
`RTEA.vcxproj`
- L882: `    <ClInclude Include="Source\CI\NetModerationGUIProbe.h" />`
- L1016: `    <ClInclude Include="Source\Menus\NetModerationGUI.h" />`
- L1464: `    <ClCompile Include="Source\CI\NetModerationGUIProbe.cpp" />`
- L1598: `    <ClCompile Include="Source\Menus\NetModerationGUI.cpp" />`
`RTEA.vcxproj.filters`
- L427: `    <ClInclude Include="Source\Menus\NetModerationGUI.h">`
- L428: `      <Filter>Menus</Filter>`
- L429: `    </ClInclude>`
- L430: `    <ClInclude Include="Source\CI\NetModerationGUIProbe.h">`
- L431: `      <Filter>System</Filter>`
- L432: `    </ClInclude>`
- L1267: `    <ClCompile Include="Source\Menus\NetModerationGUI.cpp">`
- L1268: `      <Filter>Menus</Filter>`
- L1269: `    </ClCompile>`
- L1270: `    <ClCompile Include="Source\CI\NetModerationGUIProbe.cpp">`
- L1271: `      <Filter>System</Filter>`
- L1272: `    </ClCompile>`
