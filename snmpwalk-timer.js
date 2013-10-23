var net=require('net');
var qs=require('querystring');


var HOST='127.0.0.1';
var PORT=10086;

var interval=1000*10;

//50connections, 10seconds_loop, 100fds, 300TIME_WAIT
//100connections,10seconds_loop, 200fds, 600-700TIME_WAIT

function snmpwalkLooper(intervalId){
    for(var i=120; i<121; i++){
	snmpwalk('10.41.2.'+i, '.1.3.6.1.2.1.2.2');
    }
}

snmpwalkLooper();
//setInterval(snmpwalkLooper, interval);

function snmpwalk_parse_cpu(result){
    var obj=qs.parse(result.replace(/\s=\s/g, '='), seq='\n', eq='=');
    var keywords=['ssIOSent','ssIOReceive','ssSysInterrupts','ssSysContext','ssCpuIdle',
		  'ssCpuRawUser','ssCpuRawNice','ssCpuRawSystem','ssCpuRawIdle','ssCpuRawWait',
		  'ssCpuRawKernel','ssCpuRawInterrupt','ssIORawSent','ssIORawReceived',
		  'ssRawInterrupts','ssRawContexts','ssCpuRawSoftIRQ'];


    var res={};
    keywords.forEach(function (k){
	r[k]=obj['IF-MIB::'+k+'.'+i];
    });

    return res;
}

function snmpwalk_parse_mem(result){
    var obj=qs.parse(result.replace(/\s=\s/g, '='), seq='\n', eq='=');
    var keywords=['memTotalSwap','memAvailSwap',
		  'memTotalReal','memAvailReal',
		  'memBuffer','memCached'];


    var res={};
    keywords.forEach(function (k){
	r[k]=obj['IF-MIB::'+k+'.'+i];
    });
    return res;
}
function snmpwalk_parse_if(result){

    var idx=[];
    var obj=qs.parse(result.replace(/\s=\s/g, '='), seq='\n', eq='=');
    var keywords=['ifDescr','ifMtu', 'ifSpeed', 'ifPhysAddress', 'ifType', 
		  'ifAdminStatus', 'ifOperStatus', 'ifInOctets', 'ifOutOctets'];

    var res=[];
    
    result.split('\n').forEach(function (l){
	if(l.match(/ifIndex/)){
	    idx.push(l.split('=')[1].trim());
	}
    });
    idx.forEach(function (i){
	var r={};
	keywords.forEach(function (k){
//	    console.log(obj['IF-MIB::'+k+'.'+i]);
	    r[k]=obj['IF-MIB::'+k+'.'+i];
	});
	res.push(r);
    });
    
    console.log(res);

    return res;
}

function snmpwalk(objectIP, objectOID){

    var client=new net.Socket({
	//fd:null,
	type:'tcp4',
	allowHalfOpen:true});

    client.setEncoding('utf8');
    client.setTimeout(0);
    client.setNoDelay(true);
    client.setKeepAlive(false);

    var result='';

    client.on('data', function(data){
	console.log('---data---');
	result += data;
    });

    client.on('close', function(had_error){
	console.log('---close---, error='+had_error);
	client.destroy();
	console.time();
	snmpwalk_parse_if(result);
	console.timeEnd();
    });

    client.on('error', function(error){
	console.log('Error:'+error);
    });

    client.on('end', function(){
	console.log('---end---');
    });

    client.on('timeout', function(){
	console.log('---timeout---');
	client.destroy();
    });

    client.connect(PORT, HOST, function(){
//	console.log(client.address());
	client.write('snmpwalk -c public -v 2c -r 2 -O Q '+objectIP+' '+objectOID,
		     'utf8', 
		     function(){
			 console.log('write ok');
		     });;
	client.end();
    });
}
