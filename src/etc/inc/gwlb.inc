<?php

/*
    Copyright (C) 2008 Bill Marquette <bill.marquette@gmail.com>
    Copyright (C) 2008 Seth Mos <seth.mos@dds.nl>
    Copyright (C) 2010 Ermal Luçi
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
    AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
    OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

function return_apinger_defaults()
{
    return array(
      "latencylow" => "200",
      "latencyhigh" => "500",
      "losslow" => "10",
      "losshigh" => "20",
      "interval" => "1",
      "down" => "10",
      "avg_delay_samples" => "10",
      "avg_loss_samples" => "50",
      "avg_loss_delay_samples" => "20"
    );
}

function setup_gateways_monitor($verbose = false)
{
    global $config;

    if ($verbose) {
        echo 'Setting up gateway monitors...';
        flush();
    }

    killbypid('/var/run/apinger.pid', 'TERM', true);
    @unlink('/var/run/apinger.status');

    @mkdir('/var/db/rrd', 0775);
    @chown('/var/db/rrd', 'nobody');

    $gateways_arr_all = return_gateways_array(true);

    $apinger_default = return_apinger_defaults();
    $apingerconfig = <<<EOD

# OPNsense apinger configuration file. Automatically Generated!

## User and group the pinger should run as
user "root"
group "wheel"

## Mailer to use (default: "/usr/lib/sendmail -t")
#mailer "/var/qmail/bin/qmail-inject"

## Location of the pid-file (default: "/var/run/apinger.pid")
pid_file "/var/run/apinger.pid"

## Format of timestamp (%s macro) (default: "%b %d %H:%M:%S")
#timestamp_format "%Y%m%d%H%M%S"

status {
  ## File where the status information should be written to
  file "/var/run/apinger.status"
  ## Interval between file updates
  ## when 0 or not set, file is written only when SIGUSR1 is received
  interval 5s
}

########################################
# RRDTool status gathering configuration
# Interval between RRD updates
rrd interval 60s;

## These parameters can be overridden in a specific alarm configuration
alarm default {
  command on "/usr/local/opnsense/service/configd_ctl.py -m 'dyndns reload %T' 'rfc2136 reload %T' 'openvpn reload %T'  'filter reload' "
  command off "/usr/local/opnsense/service/configd_ctl.py -m 'dyndns reload %T' 'rfc2136 reload %T' 'openvpn reload %T' 'filter reload' "
  combine 10s
}

## "Down" alarm definition.
## This alarm will be fired when target doesn't respond for 30 seconds.
alarm down "down" {
  time {$apinger_default['down']}s
}

## "Delay" alarm definition.
## This alarm will be fired when responses are delayed more than 200ms
## it will be canceled, when the delay drops below 100ms
alarm delay "delay" {
  delay_low {$apinger_default['latencylow']}ms
  delay_high {$apinger_default['latencyhigh']}ms
}

## "Loss" alarm definition.
## This alarm will be fired when packet loss goes over 20%
## it will be canceled, when the loss drops below 10%
alarm loss "loss" {
  percent_low {$apinger_default['losslow']}
  percent_high {$apinger_default['losshigh']}
}

target default {
  ## How often the probe should be sent
  interval {$apinger_default['interval']}s

  ## How many replies should be used to compute average delay
  ## for controlling "delay" alarms
  avg_delay_samples {$apinger_default['avg_delay_samples']}

  ## How many probes should be used to compute average loss
  avg_loss_samples {$apinger_default['avg_loss_samples']}

  ## The delay (in samples) after which loss is computed
  ## without this delays larger than interval would be treated as loss
  avg_loss_delay_samples {$apinger_default['avg_loss_delay_samples']}

  ## Names of the alarms that may be generated for the target
  alarms "down","delay","loss"

  ## Location of the RRD
  #rrd file "/var/db/rrd/apinger-%t.rrd"
}

EOD;

    $monitor_ips = array();
    foreach($gateways_arr_all as $name => $gateway) {
        if (empty($gateway['monitor']) || !is_ipaddr($gateway['monitor'])) {
            if (is_ipaddr($gateway['gateway'])) {
                $gateway['monitor'] = $gateway['gateway'];
            } else {
                /* No chance to get an ip to monitor skip target. */
                continue;
            }
        }

        /* if the monitor address is already used before, skip */
        if (in_array($gateway['monitor'], $monitor_ips)) {
            continue;
        }

        /*
         * Interface IP is needed since apinger will bind a socket to it.
         * However the config GUI should already have checked this and when
         * PPoE is used the IP address is set to "dynamic". So using is_ipaddrv4
         * or is_ipaddrv6 to identify packet type would be wrong, especially as
         * further checks (that can cope with the "dynamic" case) are present inside
         * the if block. So using $gateway['ipprotocol'] is the better option.
         */
        if ($gateway['ipprotocol'] == "inet") { // This is an IPv4 gateway...
            $gwifip = find_interface_ip($gateway['interface']);
            if (!is_ipaddrv4($gwifip)) {
                continue; //Skip this target
            }

            /* flush the monitor unconditionally */
            if (is_ipaddrv4($gateway['gateway']) && $gateway['monitor'] != $gateway['gateway']) {
                log_error("Removing static route for monitor {$gateway['monitor']} via {$gateway['gateway']}");
                system_host_route($gateway['monitor'], $gateway['gateway'], true, false);
            }

            /* Do not monitor if such was requested */
            if (isset($gateway['disabled']) || isset($gateway['monitor_disable'])) {
                continue;
            }

            /*
             * If the gateway is the same as the monitor we do not add a
             * route as this will break the routing table.
             * Add static routes for each gateway with their monitor IP
             * not strictly necessary but is a added level of protection.
             */
            if (is_ipaddrv4($gateway['gateway']) && $gateway['monitor'] != $gateway['gateway']) {
                log_error("Adding static route for monitor {$gateway['monitor']} via {$gateway['gateway']}");
                system_host_route($gateway['monitor'], $gateway['gateway'], false, true);
            }
        } elseif ($gateway['ipprotocol'] == "inet6") { // This is an IPv6 gateway...
            if (is_linklocal($gateway['monitor'])) {
                /* link local monitor needs a link local address for the "src" part */
                $gwifip = find_interface_ipv6_ll($gateway['interface']);
            } else {
                /* monitor is a routable address, so use a routable address for the "src" part */
                $gwifip = find_interface_ipv6($gateway['interface']);
            }

            if (!is_ipaddrv6($gwifip)) {
                /* skip this target */
                continue;
            }

            /*
             * If gateway is a local link and 'monitor' is global routable
             * then the ICMP6 response would not find its way back home.
             */
            if (is_linklocal($gateway['monitor']) && strpos($gateway['monitor'], '%') === false) {
                $gateway['monitor'] .= "%{$gateway['interface']}";
            }
            if (is_linklocal($gateway['gateway']) && strpos($gateway['gateway'], '%') === false) {
                $gateway['gateway'] .= "%{$gateway['interface']}";
            }
            if (is_linklocal($gwifip) && strpos($gwifip, '%') === false) {
                $gwifip .= "%{$gateway['interface']}";
            }

            /* flush the monitor unconditionally */
            if (is_ipaddrv6($gateway['gateway']) && $gateway['monitor'] != $gateway['gateway']) {
                log_error("Removing static route for monitor {$gateway['monitor']} via {$gateway['gateway']}");
                system_host_route($gateway['monitor'], $gateway['gateway'], true, false);
            }

            /* Do not monitor if such was requested */
            if (isset($gateway['disabled']) || isset($gateway['monitor_disable'])) {
                continue;
            }

            /*
             * If the gateway is the same as the monitor we do not add a
             * route as this will break the routing table.
             * Add static routes for each gateway with their monitor IP
             * not strictly necessary but is a added level of protection.
             */
            if (is_ipaddrv6($gateway['gateway']) && $gateway['monitor'] != $gateway['gateway']) {
                log_error("Adding static route for monitor {$gateway['monitor']} via {$gateway['gateway']}");
                system_host_route($gateway['monitor'], $gateway['gateway'], false, true);
            }
        } else {
            continue;
        }

        $monitor_ips[] = $gateway['monitor'];
        $apingercfg = "target \"{$gateway['monitor']}\" {\n";
        $apingercfg .= "  description \"{$name}\"\n";
        $apingercfg .= "  srcip \"{$gwifip}\"\n";

        ## How often the probe should be sent
        if (!empty($gateway['interval']) &&  is_numeric($gateway['interval'])) {
            $interval = intval($gateway['interval']);  # Restrict to Integer
            if ($interval <  1) {
                $interval =  1;  # Minimum
            }
            if ($interval != $apinger_default['interval'])  {
                $apingercfg .= "  interval " . $interval . "s\n"; # If not default value
            }
        }

        ## How many replies should be used to compute average delay
        ## for controlling "delay" alarms
        if (!empty($gateway['avg_delay_samples']) && is_numeric($gateway['avg_delay_samples'])) {
            $avg_delay_samples = intval($gateway['avg_delay_samples']);  # Restrict to Integer
            if ($avg_delay_samples <  1) {
                $avg_delay_samples =  1;  # Minimum
            }
            if ($avg_delay_samples != $apinger_default['avg_delay_samples']) {
                # If not default value
                $apingercfg .= "  avg_delay_samples " . $avg_delay_samples . "\n";
            }
        }

        ## How many probes should be used to compute average loss
        if (!empty($gateway['avg_loss_samples']) && is_numeric($gateway['avg_loss_samples'])) {
            $avg_loss_samples = intval($gateway['avg_loss_samples']);  # Restrict to Integer
            if ($avg_loss_samples < 1) {
                $avg_loss_samples = 1;  # Minimum
            }
            if ($avg_loss_samples != $apinger_default['avg_loss_samples']) {
                # If not default value
                $apingercfg .= "  avg_loss_samples " . $avg_loss_samples . "\n";
            }
        }

        ## The delay (in samples) after which loss is computed
        ## without this delays larger than interval would be treated as loss
        if (!empty($gateway['avg_loss_delay_samples']) && is_numeric($gateway['avg_loss_delay_samples'])) {
            $avg_loss_delay_samples = intval($gateway['avg_loss_delay_samples']);  # Restrict to Integer
            if ($avg_loss_delay_samples < 1) {
                $avg_loss_delay_samples = 1;  # Minimum
            }
            if ($avg_loss_delay_samples != $apinger_default['avg_loss_delay_samples']) {
                # If not default value
                $apingercfg .= "  avg_loss_delay_samples " . $avg_loss_delay_samples . "\n";
            }
        }

        $alarms = "";
        $alarmscfg = "";
        $override = false;
        if (!empty($gateway['losslow'])) {
            $alarmscfg .= "alarm loss \"{$name}loss\" {\n";
            $alarmscfg .= "\tpercent_low {$gateway['losslow']}\n";
            $alarmscfg .= "\tpercent_high {$gateway['losshigh']}\n";
            $alarmscfg .= "}\n";
            $alarms .= "\"{$name}loss\"";
            $override = true;
        } else {
            if ($override == true) {
                $alarms .= ",";
            }
            $alarms .= "\"loss\"";
            $override = true;
        }
        if (!empty($gateway['latencylow'])) {
            $alarmscfg .= "alarm delay \"{$name}delay\" {\n";
            $alarmscfg .= "\tdelay_low {$gateway['latencylow']}ms\n";
            $alarmscfg .= "\tdelay_high {$gateway['latencyhigh']}ms\n";
            $alarmscfg .= "}\n";
            if ($override == true) {
                $alarms .= ",";
            }
            $alarms .= "\"{$name}delay\"";
            $override = true;
        } else {
            if ($override == true) {
                $alarms .= ",";
            }
            $alarms .= "\"delay\"";
            $override = true;
        }
        if (!empty($gateway['down'])) {
            $alarmscfg .= "alarm down \"{$name}down\" {\n";
            $alarmscfg .= "\ttime {$gateway['down']}s\n";
            $alarmscfg .= "}\n";
            if ($override == true) {
                $alarms .= ",";
            }
            $alarms .= "\"{$name}down\"";
            $override = true;
        } else {
            if ($override == true) {
                $alarms .= ",";
            }
            $alarms .= "\"down\"";
            $override = true;
        }
        if ($override == true) {
            $apingercfg .= "\talarms override {$alarms};\n";
        }

        if (isset($gateway['force_down'])) {
            $apingercfg .= "\tforce_down on\n";
        }

        $apingercfg .= "  rrd file \"/var/db/rrd/{$gateway['name']}-quality.rrd\"\n";
        $apingercfg .= "}\n";
        $apingercfg .= "\n";

        $apingerconfig .= $alarmscfg;
        $apingerconfig .= $apingercfg;

        # Create gateway quality RRD with settings more suitable for OPNsense graph set,
        # since apinger uses default step (300; 5 minutes) and other settings that don't
        # match the OPNsense gateway quality graph set.
        rrd_create_gateway_quality("/var/db/rrd/{$gateway['name']}-quality.rrd");
    }

    $gateways_arr = return_gateways_array();
    if (!is_array($gateways_arr)) {
        log_error("No gateways to monitor. Apinger will not be run.");
    } else {
        file_put_contents('/var/etc/apinger.conf', $apingerconfig);
        mwexec_bg('/usr/local/sbin/apinger -c /var/etc/apinger.conf');
    }

    if ($verbose) {
        echo "done.\n";
    }
}

/* return the status of the apinger targets as a array */
function return_gateways_status($byname = false)
{
    global $config;

    $status = array();

    if (isvalidpid('/var/run/apinger.pid')) {
        /* always get the latest status from apinger */
        killbypid('/var/run/apinger.pid', 'USR1');

        /* we may read the wrong file here as it's async: */
        if (file_exists('/var/run/apinger.status')) {
            $apingerstatus = file('/var/run/apinger.status');

            foreach ($apingerstatus as $line) {
                $info = explode('|', $line);
                if ($byname == false) {
                    $target = $info[0];
                } else {
                    $target = $info[2];
                }

                $status[$target] = array();
                $status[$target]['monitorip'] = $info[0];
                $status[$target]['srcip'] = $info[1];
                $status[$target]['name'] = $info[2];
                $status[$target]['delay'] = sprintf('%0.1f ms', empty($info[6]) ? 0.0 : round($info[6], 1));
                $status[$target]['loss'] = sprintf('%0.1f %%', empty($info[7]) ? 0.0 : round($info[7], 1));
                $status[$target]['status'] = trim($info[8]);
            }
        }
    }

    /* tack on any gateways that have monitoring disabled
     * or are down, which could cause gateway groups to fail */
    $gateways_arr = return_gateways_array();
    foreach($gateways_arr as $gwitem) {
        if (!isset($gwitem['monitor_disable'])) {
            continue;
        }
        if (!isset($gwitem['monitorip']) || !is_ipaddr($gwitem['monitorip'])) {
            $realif = $gwitem['interface'];
            $tgtip = get_interface_gateway($realif);
            if (!is_ipaddr($tgtip)) {
                $tgtip = "none";
            }
            $srcip = find_interface_ip($realif);
        } else {
            $tgtip = $gwitem['monitorip'];
            $srcip = find_interface_ip($realif);
        }
        if ($byname == true) {
            $target = $gwitem['name'];
        } else {
            $target = $tgtip;
        }

        /* failsafe for down interfaces */
        if ($target == 'none') {
            $target = $gwitem['name'];
            $status[$target]['name'] = $gwitem['name'];
            $status[$target]['delay'] = '0.0 ms';
            $status[$target]['loss'] = '100.0 %';
            $status[$target]['status'] = 'down';
        } else {
            $status[$target]['monitorip'] = $tgtip;
            $status[$target]['srcip'] = $srcip;
            $status[$target]['name'] = $gwitem['name'];
            $status[$target]['delay'] = '0.0 ms';
            $status[$target]['loss'] = '0.0 %';
            $status[$target]['status'] = 'none';
        }

    }
    return($status);
}

/* Return all configured gateways on the system */
function return_gateways_array($disabled = false, $localhost = false, $inactive = false)
{
    global $config;

    $gateways_arr = array();

    $found_defaultv4 = 0;
    $found_defaultv6 = 0;

    $iflist = get_configured_interface_with_descr();

    /* Process/add dynamic v4 gateways. */
    foreach ($iflist as $ifname => $friendly) {
        if (!interface_has_gateway($ifname)) {
            continue;
        }

        $ifcfg = &config_read_array('interfaces', $ifname);

        if (!empty($ifcfg['ipaddr']) && is_ipaddrv4($ifcfg['ipaddr'])) {
            continue;
        }

        $ctype = '';
        switch($ifcfg['ipaddr']) {
            case "dhcp":
            case "pppoe":
            case "pptp":
            case "ppp":
                $ctype = $ifcfg['ipaddr'];
                break;
            default:
                if (substr($ifcfg['if'], 0, 4) ==  "ovpn") {
                    // if current iface is an ovpn server endpoint then skip it
                    if (substr($ifcfg['if'], 4, 1) == 's') {
                        continue 2;
                    }

                    $ctype = "VPNv4";
                }
                break;
        }

        if (empty($ctype)) {
            continue;
        }

        $ctype = "_". strtoupper($ctype);

        $gateway = array();
        $gateway['dynamic'] = false;
        $gateway['ipprotocol'] = "inet";
        $gateway['gateway'] = get_interface_gateway($ifname, $gateway['dynamic']);
        $gateway['interface'] = get_real_interface($ifname);
        $gateway['friendlyiface'] = $ifname;
        $gateway['name'] = "{$friendly}{$ctype}";
        $gateway['attribute'] = "system";

        if ($gateway['dynamic'] === "default" && $found_defaultv4 == 0) {
            $gateway['defaultgw'] = true;
            $gateway['dynamic'] = true;
            $found_defaultv4 = 1;
        }
        /* Loopback dummy for dynamic interfaces without a IP */
        if (!is_ipaddrv4($gateway['gateway']) && $gateway['dynamic'] == true) {
            $gateway['gateway'] = "dynamic";
        }

        if (is_ipaddrv4($gateway['gateway'])) {
            $gateway['monitor'] = $gateway['gateway'];
        }

        $gateway['descr'] = "Interface {$friendly}{$ctype} Gateway";
        $gateway['monitor_disable'] = true;
        $gateways_arr[$gateway['name']] = $gateway;
    }

    /* Process/add dynamic v6 gateways. */
    foreach ($iflist as $ifname => $friendly) {
        /* If the user has disabled IPv6, they probably don't want any IPv6 gateways. */
        if (!isset($config['system']['ipv6allow'])) {
            break;
        }

        if (!interface_has_gatewayv6($ifname)) {
            continue;
        }

        if (empty($config['interfaces'][$ifname])) {
            continue;
        }

        $ifcfg = &config_read_array('interfaces', $ifname);

        if (!empty($ifcfg['ipaddrv6']) && is_ipaddrv6($ifcfg['ipaddrv6'])) {
            continue;
        }

        $ctype = '';
        switch($ifcfg['ipaddrv6']) {
            case "slaac":
            case "dhcp6":
            case "6to4":
            case "6rd":
                $ctype = $ifcfg['ipaddrv6'];
                break;
            default:
                $tunnelif = substr($ifcfg['if'], 0, 3);
                if (substr($ifcfg['if'], 0, 4) ==  "ovpn") {
                    // if current iface is an ovpn server endpoint then skip it
                    if (substr($ifcfg['if'], 4, 1) == 's') {
                        continue 2;
                    }

                    $ctype = "VPNv6";
                } elseif ($tunnelif == "gif" || $tunnelif == "gre") {
                    $ctype = "TUNNELv6";
                }
                break;
        }

        if (empty($ctype)) {
            continue;
        }

        $ctype = "_". strtoupper($ctype);

        $gateway = array();
        $gateway['dynamic'] = false;
        $gateway['ipprotocol'] = "inet6";
        $gateway['gateway'] = get_interface_gateway_v6($ifname, $gateway['dynamic']);
        $gateway['interface'] = get_real_interface($ifname, "inet6");
        switch($ifcfg['ipaddrv6']) {
            case "6rd":
            case "6to4":
                $gateway['dynamic'] = "default";
                break;
        }
        $gateway['friendlyiface'] = $ifname;
        $gateway['name'] = "{$friendly}{$ctype}";
        $gateway['attribute'] = "system";

        if (($gateway['dynamic'] === "default")  && ($found_defaultv6 == 0)) {
            $gateway['defaultgw'] = true;
            $gateway['dynamic'] = true;
            $found_defaultv6 = 1;
        }

        /* Loopback dummy for dynamic interfaces without a IP */
        if (!is_ipaddrv6($gateway['gateway']) && $gateway['dynamic'] == true) {
            $gateway['gateway'] = "dynamic";
        }

        if (is_ipaddrv6($gateway['gateway'])) {
            $gateway['monitor'] = $gateway['gateway'];
        }

        $gateway['descr'] = "Interface {$friendly}{$ctype} Gateway";
        $gateway['monitor_disable'] = true;
        $gateways_arr[$gateway['name']] = $gateway;
    }

    $i = 0;
    /* Process/add all the configured gateways. */
    if (isset($config['gateways']['gateway_item'])) {
        foreach ($config['gateways']['gateway_item'] as $gateway) {
            if (empty($config['interfaces'][$gateway['interface']])) {
                if (!$inactive) {
                    continue;
                } else {
                    $gateway['inactive'] = true;
                }
            }
            $wancfg = $config['interfaces'][$gateway['interface']];

            /* skip disabled interfaces */
            if (!$disabled && (!isset($wancfg['enable']) || isset($gateway['disabled']))) {
                if (isset($gateways_arr[$gateway['name']])) {
                    unset($gateways_arr[$gateway['name']]);
                }
                continue;
            }

            /* if the gateway is dynamic and we can find the IPv4, Great! */
            if (empty($gateway['gateway']) || $gateway['gateway'] == "dynamic") {
                if ($gateway['ipprotocol'] == "inet") {
                    /* we know which interfaces is dynamic, this should be made a function */
                    $gateway['gateway'] = get_interface_gateway($gateway['interface']);
                    /* no IP address found, set to dynamic */
                    if (!is_ipaddrv4($gateway['gateway'])) {
                        $gateway['gateway'] = "dynamic";
                    }
                    $gateway['dynamic'] = true;
                } elseif ($gateway['ipprotocol'] == "inet6") {
                    /* if the gateway is dynamic and we can find the IPv6, Great! */
                    /* we know which interfaces is dynamic, this should be made a function, and for v6 too */
                    $gateway['gateway'] = get_interface_gateway_v6($gateway['interface']);
                    /* no IPv6 address found, set to dynamic */
                    if (!is_ipaddrv6($gateway['gateway'])) {
                        $gateway['gateway'] = "dynamic";
                    }
                    $gateway['dynamic'] = true;
                }
            }

            if (isset($gateway['monitor_disable'])) {
                $gateway['monitor_disable'] = true;
            } elseif (empty($gateway['monitor'])) {
                $gateway['monitor'] = $gateway['gateway'];
            }

            $gateway['friendlyiface'] = $gateway['interface'];

            /* special treatment for tunnel interfaces */
            if ($gateway['ipprotocol'] == "inet6") {
                $gateway['interface'] = get_real_interface($gateway['interface'], "inet6", false, false);
            } else {
                $gateway['interface'] = get_real_interface($gateway['interface'], "all", false, false);
            }

            /* entry has a default flag, use it */
            if (isset($gateway['defaultgw'])) {
                $gateway['defaultgw'] = true;
            }
            /* include the gateway index as the attribute */
            $gateway['attribute'] = $i++;

            $gateways_arr[$gateway['name']] = $gateway;
        }
    }

    if ($localhost === true) {
        /* attach localhost for Null routes */
        $gwlo4 = array();
        $gwlo4['name'] = "Null4";
        $gwlo4['interface'] = "lo0";
        $gwlo4['ipprotocol'] = "inet";
        $gwlo4['gateway'] = "127.0.0.1";
        $gwlo6 = array();
        $gwlo6['name'] = "Null6";
        $gwlo6['interface'] = "lo0";
        $gwlo6['ipprotocol'] = "inet6";
        $gwlo6['gateway'] = "::1";
        $gateways_arr['Null4'] = $gwlo4;
        $gateways_arr['Null6'] = $gwlo6;
    }

    return $gateways_arr;
}

function fixup_default_gateway($gateways_status, $gateways_arr)
{
    foreach (array("inet", "inet6") as $ipprotocol) {
        $dfltgwname = null;
        $count = 0;

        foreach ($gateways_arr as $gwname => $gwsttng) {
            if (isset($gwsttng['disabled'])) {
                /* does not apply */
                continue;
            }
            if ($gwsttng['ipprotocol'] !== $ipprotocol) {
                /* wrong address family */
                continue;
            }
            if (!isset($gwsttng['monitoring_disabled']) && $gateways_status[$gwname]['status'] === 'down') {
                /* cannot use down gateway */
                continue;
            }

            /* can use this gateway */
            $dfltgwname = $gwname;
            $count += 1;

            if (isset($gwsttng['defaultgw'])) {
                /* if we found the default to be working use it right away */
                break;
            }
        }

        if (empty($dfltgwname)) {
            if ($count) {
                log_error("Cannot switch while all $count $ipprotocol gateways are down");
            }
            continue;
        }

        if ($gateways_arr[$dfltgwname]['gateway'] == "dynamic") {
            if ($ipprotocol == 'inet') {
                $gwip = get_interface_gateway($gateways_arr[$dfltgwname]['friendlyiface']);
            } else {
                $gwip = get_interface_gateway_v6($gateways_arr[$dfltgwname]['friendlyiface']);
            }
        } else {
            $gwip = $gateways_arr[$dfltgwname]['gateway'];
        }

        if (!is_ipaddr($gwip)) {
            log_error("Cannot switch gateway $dfltgwname with undefined address");
            continue;
        }

        $defaultif = get_real_interface($gateways_arr[$dfltgwname]['friendlyiface']);
        $gwipmatch = $gwip;
        if (is_linklocal($gwip)) {
            /* correct match in IPv6 case */
            $gwipmatch .= "%{$defaultif}";
        }

        $tmpcmd = "/sbin/route -n get -{$ipprotocol} default 2>/dev/null | /usr/bin/awk '/gateway:/ {print $2}'";
        $defaultgw = trim(exec($tmpcmd), " \n");
        if ($defaultgw != $gwipmatch) {
            log_error("Switching default gateway to $dfltgwname ($gwip)");
            /* XXX fargw and IPv6 should be cleaned up to make it easier to read */
            system_default_route($gwip, !isset($gateways_arr[$dfltgwname]['fargw']) && $ipprotocol == 'inet' ? null : $defaultif);
        }
    }
}

/*
 * Return an array with all gateway groups with name as key
 * All gateway groups will be processed before returning the array.
 */
function return_gateway_groups_array()
{
    global $config;

    /* fetch the current gateways status */
    $gateways_status = return_gateways_status(true);
    $gateways_arr = return_gateways_array();
    $gateway_groups_array = array();

    if (isset($config['gateways']['gateway_group'])) {
        $carplist = get_configured_carp_interface_list();
        foreach ($config['gateways']['gateway_group'] as $group) {
            /* create array with group gateways members separated by tier */
            $tiers = array();
            $tiers_online = 0;
            foreach ($group['item'] as $item) {
                list($gwname, $tier, $vipname) = explode("|", $item);
                if (!isset($tiers[$tier])) {
                    $tiers[$tier] = array();
                }

                $gwup = false;
                if (!empty($gateways_status[$gwname])) {
                    $status = $gateways_status[$gwname];
                    if (stristr($status['status'], 'down')) {
                        $msg = sprintf(gettext('MONITOR: %s is down, removing from routing group %s'), $gwname, $group['name']);
                    } elseif (stristr($status['status'], 'loss') && strstr($group['trigger'], 'loss')) {
                        $msg = sprintf(gettext('MONITOR: %s has packet loss, removing from routing group %s'), $gwname, $group['name']);
                    } elseif (stristr($status['status'], 'delay') && strstr($group['trigger'] , 'latency')) {
                        $msg = sprintf(gettext('MONITOR: %s has high latency, removing from routing group %s'), $gwname, $group['name']);
                    } else {
                        $msg = null;
                    }
                    if (!empty($msg)) {
                        log_error($msg);
                        notify_all_remote($msg);
                    } else {
                        $tiers_online++;
                        $gwup = true;
                    }
                } elseif (isset($gateways_arr[$gwname]['monitor_disable'])) {
                    $gwup = true;
                }
                $vip = null;
                if (!empty($carplist[$vipname]) && is_ipaddr($carplist[$vipname])) {
                    $vip = $vipname;
                }

                $tiers[$tier][] = array("up" => $gwup, "gwname" => $gwname, "vip" => $vip);
            }
            if ($tiers_online == 0 && !file_exists('/var/run/booting')) {
                /* Oh dear, we have no members!*/
                $msg = sprintf(gettext('Gateways status could not be determined, considering all as up/active. (Group: %s)'), $group['name']);
                log_error($msg);
                notify_via_growl($msg);
            }

            /* sort the tiers array by the tier key */
            ksort($tiers);

            /* we do not really foreach the tiers as we stop after the first tier */
            foreach ($tiers as $tieridx => $tier) {
                /* process all gateways in this tier */
                foreach ($tier as $member_item) {
                    $member = $member_item['gwname'];
                    if (($member_item['up'] || $tiers_online == 0) && isset($gateways_arr[$member])) {
                        /* determine interface gateway */
                        $gateway = $gateways_arr[$member];
                        if (is_ipaddr($gateway['gateway'])) {
                            $gatewayip = $gateway['gateway'];
                        } elseif (!empty($gateway['interface'])) {
                            $gatewayip = get_interface_gateway($gateway['friendlyiface']);
                        } else {
                            $gatewayip = "";
                        }

                        if (!empty($gateway['interface'])) {
                            if (empty($gateway_groups_array[$group['name']])) {
                                $gateway_groups_array[$group['name']] = array();
                            }
                            if (is_ipaddr($gatewayip)) {
                                $groupmember = array();
                                $groupmember['int']  = $gateway['interface'];
                                $groupmember['gwip']  = $gatewayip;
                                $groupmember['weight']  = isset($gateway['weight']) ? $gateway['weight'] : 1;
                                if (!empty($member_item['vip'])) {
                                    $groupmember['vip'] = $member_item['vip'];
                                }
                                $gateway_groups_array[$group['name']][] = $groupmember;
                            }
                        }
                    }
                }

                /* we should have the 1st available tier now, exit stage left */
                if (count($gateway_groups_array[$group['name']]) > 0) {
                    break;
                } else {
                    log_error("GATEWAYS: Group {$group['name']} did not have any gateways up on tier {$tieridx}!");
                }
            }
        }
    }

    return $gateway_groups_array;
}

function lookup_gateway_ip_by_name($name)
{
    $gateways_arr = return_gateways_array(true, true);

    foreach ($gateways_arr as $gname => $gw) {
        if ($gw['name'] === $name || $gname === $name) {
            if (is_linklocal($gw['gateway']) && strpos($gw['gateway'], '%') === false) {
                $gw['gateway'] .= "%{$gw['interface']}";
            }
            return $gw['gateway'];
        }
    }

    return false;
}

function lookup_gateway_interface_by_name($name)
{
    $gateways_arr = return_gateways_array(false, true);
    if (!empty($gateways_arr[$name])) {
        $interfacegw = $gateways_arr[$name]['friendlyiface'];
        return ($interfacegw);
    }

    return (false);
}

function get_interface_gateway($interface, &$dynamic = false)
{
    global $config;

    $gw = NULL;

    if (isset($config['interfaces'][$interface])) {
        $gwcfg = $config['interfaces'][$interface];
        if (!empty($gwcfg['gateway']) && isset($config['gateways']['gateway_item'])) {
            foreach($config['gateways']['gateway_item'] as $gateway) {
                if (($gateway['name'] == $gwcfg['gateway']) && (is_ipaddrv4($gateway['gateway']))) {
                    $gw = $gateway['gateway'];
                    break;
                }
            }
        }

        // for dynamic interfaces we handle them through the $interface_router file.
        if (!is_ipaddrv4($gw) && !is_ipaddrv4($gwcfg['ipaddr'])) {
            $realif = get_real_interface($interface);
            if (file_exists("/tmp/{$realif}_router")) {
                $gw = trim(file_get_contents("/tmp/{$realif}_router"), " \n");
                $dynamic = true;
            }
            if (file_exists("/tmp/{$realif}_defaultgw")) {
                $dynamic = "default";
            }
        }
    }

    /* return gateway */
    return ($gw);
}

function get_interface_gateway_v6($interface, &$dynamic = false)
{
    global $config;

    $gw = NULL;
    $gwcfg = $config['interfaces'][$interface];
    if (!empty($gwcfg['gatewayv6']) && isset($config['gateways']['gateway_item'])) {
        foreach($config['gateways']['gateway_item'] as $gateway) {
            if (($gateway['name'] == $gwcfg['gatewayv6']) && (is_ipaddrv6($gateway['gateway']))) {
                $gw = $gateway['gateway'];
                break;
            }
        }
    }

    // for dynamic interfaces we handle them through the $interface_router file.
    if (!is_ipaddrv6($gw) && ( !isset($gwcfg['ipaddrv6']) || !is_ipaddrv6($gwcfg['ipaddrv6']))) {
          $realif = get_real_interface($interface);
          if (file_exists("/tmp/{$realif}_routerv6")) {
              $gw = trim(file_get_contents("/tmp/{$realif}_routerv6"), " \n");
              $dynamic = true;
          }
          if (file_exists("/tmp/{$realif}_defaultgwv6")) {
              $dynamic = "default";
          }
    }
    /* return gateway */
    return ($gw);
}

/* Check a IP address against a gateway IP or name
 * to verify it's address family */
function validate_address_family($ipaddr, $gwname)
{
    $v4ip = false;
    $v6ip = false;
    $v4gw = false;
    $v6gw = false;

    if (is_ipaddrv4($ipaddr)) {
        $v4ip = true;
    }
    if (is_ipaddrv6($ipaddr)) {
        $v6ip = true;
    }
    if (is_ipaddrv4($gwname)) {
        $v4gw = true;
    }
    if (is_ipaddrv6($gwname)) {
        $v6gw = true;
    }

    if ($v4ip && $v4gw) {
        return true;
    }
    if ($v6ip && $v6gw) {
        return true;
    }

    /* still no match, carry on, lookup gateways */
    if (is_ipaddrv4(lookup_gateway_ip_by_name($gwname))) {
        $v4gw = true;
    }
    if (is_ipaddrv6(lookup_gateway_ip_by_name($gwname))) {
        $v6gw = true;
    }

    $gw_array = return_gateways_array();
    if (is_array($gw_array[$gwname])) {
        switch($gw_array[$gwname]['ipprotocol']) {
            case "inet":
                $v4gw = true;
                break;
            case "inet6":
                $v6gw = true;
                break;
        }
    }

    if ($v4ip && $v4gw) {
        return true;
    }
    if ($v6ip && $v6gw) {
        return true;
    }

    return false;
}
