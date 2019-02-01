<!DOCTYPE HTML PUBLIC '-//W3C//DTD HTML 4.0//EN'>
<!--
	Tomato GUI
	Copyright (C) 2006-2010 Jonathan Zarate
	http://www.polarcloud.com/tomato/

    SMCRoute GUI
    Copyright (C) 2019 Kelvin You

	For use with Tomato Firmware only.
	No part of this file may be used without permission.
-->
<html>
<head>
<meta http-equiv='content-type' content='text/html;charset=utf-8'>
<meta name='robots' content='noindex,nofollow'>
<title>[<% ident(); %>] Advanced: Multicast Route</title>
<link rel='stylesheet' type='text/css' href='tomato.css'>
<link rel='stylesheet' type='text/css' href='color.css'>
<script type='text/javascript' src='tomato.js'></script>

<!-- / / / -->

<style type='text/css'>
textarea {
	width: 98%;
	height: 15em;
}
</style>

<script type='text/javascript' src='debug.js'></script>

<script type='text/javascript'>
//	<% nvram("smc_enable,smc_config,smc_firewall"); %>

smcup = parseInt ('<% psup("smcrouted"); %>');

function verifyFields(focused, quiet)
{
	return 1;
}

function toggle(service, isup)
{
    E('_' + service + '_button1').disabled = true;
    form.submitHidden('/service.cgi', {
        _redirect: 'advanced-smcroute.asp',
        _sleep: ((service == 'smcroute') && (!isup)) ? '3' : '3',
        _service: service + (isup ? '-stop' : '-start')
    });
}

function save()
{
	var fom;
	
	fom = E('_fom');
    
    if ( smcup ) 
    {
        fom._service.value = 'smcroute-restart';
    }

    fom.smc_enable.value = fom.f_smc_enable.checked ? 1 : 0;
	form.submit(fom, 1);
}

function earlyInit()
{
}
</script>
</head>
<body>
<form id='_fom' method='post' action='tomato.cgi'>
<table id='container' cellspacing=0>
<tr><td colspan=2 id='header'>
	<div class='title'>Tomato</div>
	<div class='version'>Version <% version(); %></div>
</td></tr>
<tr id='body'><td id='navi'><script type='text/javascript'>navi()</script></td>
<td id='content'>
<div id='ident'><% ident(); %></div>

<!-- / / / -->

<input type='hidden' name='_nextpage' value='advanced-smcroute.asp'>
<input type='hidden' name='_service' value=''>
<input type='hidden' name='smc_enable'>

<div class='section-title'>Multicast Route</div>
<div class='section'>
<script type='text/javascript'>
createFieldTable('', [
    { title: 'Enable', name: 'f_smc_enable', type: 'checkbox', value: (nvram.smc_enable == 1) },
    { title: 'Configuration', name: 'smc_config', type: 'textarea', value: nvram.smc_config },
    { title: 'Firewall Script', name: 'smc_firewall', type: 'textarea', value: nvram.smc_firewall }    
]);
W('<input type="button" value="' + (smcup ? 'Stop' : 'Start') + ' Now" onclick="toggle(\'smcroute\', smcup)" id="_smcroute_button1">');
</script>
</div>
</div>

</div>

<!-- / / / -->

</td></tr>
<tr><td id='footer' colspan=2>
	<span id='footer-msg'></span>
	<input type='button' value='Save' id='save-button' onclick='save()'>
	<input type='button' value='Cancel' id='cancel-button' onclick='javascript:reloadPage();'>
</td></tr>
</table>
</form>
<script type='text/javascript'>earlyInit()</script>
</body>
</html>

