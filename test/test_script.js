const deviceName = 'Hardware_Password_Manager'
const bleService = '6E400001-B5A3-F393-E0A9-E50E24DCCA9E'.toLowerCase()
const bleTxCharacteristic = '6E400003-B5A3-F393-E0A9-E50E24DCCA9E'.toLowerCase()
const bleRxCharacteristic = '6E400002-B5A3-F393-E0A9-E50E24DCCA9E'.toLowerCase()

const MAX_PACKET_SIZE = 61
const MAX_STORABLE_PWD = 24
const URL_SIZE = 47
const USERNAME_SIZE = 23
const PWD_SIZE = 23

const longUrl = 'https://bluetooth_hardware_password_manager.com'
const longUsername = 'username@bhpmtest.com'
const longPwd = 'extremelylongpassword'

var start_test_button
var table

var test_list = new Array()
var test_step_list
var current_test = 0
var successfulTests = 0
var storedPasswords = 0

var deviceDetected
var expectedResponse

window.onload = function(){
    start_test_button = document.getElementById('start_test_button')
    start_test_button.onclick = startTest

    addTest(test_connect,
        'Choose BHPM module inside WebBluetooth pop-up to connect to',
        'Connection and obtaining characteristics')

    addTest(test_emptyStorage,
        '',
        'Response to a GET password request must be an error if storage is empty')

    addTest(test_storeOnePwdRejected,
        'Type \'n\' in the UART input',
        'STORE password request rejected by user')
    
    addTest(test_storeOnePwd,
        'Type \'Y\' in the UART input',
        'STORE password request success')

    addTest(test_getOnePwdRejected,
        'Type \'n\' in the UART input',
        'GET password request rejected by user')

    addTest(test_getOnePwd,
        'Type \'Y\' in the UART input',
        'GET password request success')

    addTest(test_fillStore,
        'Type \'Y\' in the UART input until test finishes',
        'Exactly ' + MAX_STORABLE_PWD + " passwords () can be stored. Performed with maximum size passwords")        

    test_step_list = document.getElementsByClassName('test_step')
}

// Add test to the test list, given callback, steps text and test description
function addTest(fun, steps, description){
    let row = document.createElement('tr')
    let step_column = document.createElement('td')
    let description_column = document.createElement('td')

    step_column.textContent = steps
    step_column.style.color = 'gray'
    step_column.style.opacity = '0.5'
    step_column.className = 'test_step'

    description_column.textContent = description;

    row.appendChild(step_column)
    row.appendChild(description_column)

    table = document.getElementById("test_steps_table")
    table.appendChild(row)

    test_list.push(fun)
}

// Start next test avaiable
function startTest(){
    // Update current step
    test_step_list[current_test].style.color = 'black'
    test_step_list[current_test].style.opacity = 1.0
    // Execute next test
    test_list[current_test]();
}

// Informs the user if the test has been passed, an starts the next test (if any)
function endTest(result){
    let symbol = document.createElement('span')
    if(result == 0){
        // Test passed successfully
        symbol.className = 'tick'
        symbol.innerHTML = ' &#10004'
        successfulTests++
    }else{
        // Test not passed
        symbol.className = 'cross'
        symbol.innerHTML = ' &#10008'
    }
    // Add symbol to the tested use case
    table.rows[current_test + 1].cells[1].appendChild(symbol)

    current_test++
    // Check if there are still tests to be done
    if(current_test < test_list.length) startTest()
    else{
        document.getElementById('finalMessage').innerHTML = 'Successfully passed tests: ' + successfulTests + '/' + test_list.length
        deviceDetected.gatt.disconnect()
    }
}

// TEST CALLBACK FUNCTIONS
// Test connection and obtaining characteristics
function test_connect(){
    let options = {
        filters:[
            {name: deviceName,
            services: [bleService]}
        ]
    }

    navigator.bluetooth.requestDevice(options)
        .then(device => {
            deviceDetected = device
            console.log("Device name: " + deviceDetected.name)
            return deviceDetected.gatt.connect()
        })
        // Enable notifications
        .then(server =>{
            return server.getPrimaryService(bleService)
        })
        .then(service => {
            return service.getCharacteristic(bleTxCharacteristic)
        })
        .then(charasteristic => charasteristic.startNotifications())
        .then(charasteristic =>{
            charasteristic.addEventListener('characteristicvaluechanged', wb_receiveNotification);
            console.log("Notifications have been started");

            endTest(0)
        })
        .catch(error => {
            console.log("test_connect error: " + error)
            endTest(error)
        })
}

// Test that response to a GET password request must be an error if storage is empty
function test_emptyStorage(){
    expectedResponse = '{"err": "operation rejected"}'
    deviceDetected.gatt.connect()
    .then(server => server.getPrimaryService(bleService))
    .then(service => service.getCharacteristic(bleRxCharacteristic))
    .then(characteristic => {
        let url = "https://test.com"
        let user = "user@test.com"
        let msg = infoToJSON(url, user, null)
        let packets = splitString(msg, MAX_PACKET_SIZE)
        wb_sendPackets(characteristic, packets, 0)
    })
    .catch(error => {
        console.log("test_emptyStorage error: " + error)
    })
}

// Test STORE password request rejected by user
function test_storeOnePwdRejected(){
    expectedResponse = '{"err": "operation rejected"}'
    deviceDetected.gatt.connect()
    .then(server => server.getPrimaryService(bleService))
    .then(service => service.getCharacteristic(bleRxCharacteristic))
    .then(characteristic => {
        let url = "https://test.com"
        let user = "user@test.com"
        let pwd = "1231424"
        let msg = infoToJSON(url, user, pwd)
        let packets = splitString(msg, MAX_PACKET_SIZE)
        wb_sendPackets(characteristic, packets, 0)
    })
    .catch(error => {
        console.log("test_storeOnePwd error: " + error)
    })
}

// Test STORE password request success
function test_storeOnePwd(){
    expectedResponse = '{"err": "ok"}'
    deviceDetected.gatt.connect()
    .then(server => server.getPrimaryService(bleService))
    .then(service => service.getCharacteristic(bleRxCharacteristic))
    .then(characteristic => {
        let url = "https://test.com"
        let user = "user@test.com"
        let pwd = "1234567890A"
        let msg = infoToJSON(url, user, pwd)
        let packets = splitString(msg, MAX_PACKET_SIZE)
        wb_sendPackets(characteristic, packets, 0)
    })
    .catch(error => {
        console.log("test_storeOnePwd error: " + error)
    })
}

// Test GET password request rejected by user
function test_getOnePwdRejected(){
    expectedResponse = '{"err": "operation rejected"}'
    deviceDetected.gatt.connect()
    .then(server => server.getPrimaryService(bleService))
    .then(service => service.getCharacteristic(bleRxCharacteristic))
    .then(characteristic => {
        let url = "https://test.com"
        let user = "user@test.com"
        let msg = infoToJSON(url, user, null)
        let packets = splitString(msg, MAX_PACKET_SIZE)
        wb_sendPackets(characteristic, packets, 0)
    })
    .catch(error => {
        console.log("test_storeOnePwd error: " + error)
    })
}

// Test GET password request success
function test_getOnePwd(){
    expectedResponse = '{"pwd": "1234567890A"}'
    deviceDetected.gatt.connect()
    .then(server => server.getPrimaryService(bleService))
    .then(service => service.getCharacteristic(bleRxCharacteristic))
    .then(characteristic => {
        let url = "https://test.com"
        let user = "user@test.com"
        let msg = infoToJSON(url, user, null)
        let packets = splitString(msg, MAX_PACKET_SIZE)
        wb_sendPackets(characteristic, packets, 0)
    })
    .catch(error => {
        console.log("test_storeOnePwd error: " + error)
    })
}

// Test what happens when exactly 24 passwords can be stored (the first one was previously stored)
function test_fillStore(){
    expectedResponse = '{"err": "ok"}'
    deviceDetected.gatt.connect()
    // Enable notifications
    .then(server =>{
        return server.getPrimaryService(bleService)
    })
    .then(service => {
        return service.getCharacteristic(bleTxCharacteristic)
    })
    .then(charasteristic => charasteristic.startNotifications())
    .then(charasteristic =>{
        charasteristic.removeEventListener('characteristicvaluechanged',wb_receiveNotification)
        charasteristic.addEventListener('characteristicvaluechanged', wb_receiveNotification_fillStore)
        console.log("Notifications event listener has changed. Notifications have been started")
    })
    .catch(error => {
        console.log("test_connect error: " + error)
        endTest(error)
    })
    
    aux_storeOneMorePwd();
}

// Auxiliar function used for store one of the 24 passwords that can be stored
function aux_storeOneMorePwd(){
    deviceDetected.gatt.connect()
    .then(server => server.getPrimaryService(bleService))
    .then(service => service.getCharacteristic(bleRxCharacteristic))
    .then(characteristic => {
        let url = longUrl
        let user = storedPasswords + longUsername
        let pwd = longPwd+storedPasswords
        let msg = infoToJSON(url, user, pwd)
        let packets = splitString(msg, MAX_PACKET_SIZE)
        wb_sendPackets(characteristic, packets, 0)
    })
    .catch(error => {
        console.log("test_storeOnePwd error: " + error)
    })
}

// EVENT LISTENER
// Generic event listener
function wb_receiveNotification(event){
    if(!("TextDecoder" in window)){
        alert("This browser does not support TextDecoder")
    }
    var encoder = new TextDecoder("utf-8")
    const valueReceived = event.target.value
    const value = encoder.decode(valueReceived)

    const response = JSON.parse(value)
    const expected = JSON.parse(expectedResponse)

    if(expected.hasOwnProperty('err') && response.hasOwnProperty('err')
        && (expected['err'] == response['err'])){
            endTest(0)
            if(response['err'] == 'ok') storedPasswords++
        }
    else if(expected.hasOwnProperty('pwd') && response.hasOwnProperty('pwd')
        && (expected['pwd'] == response['pwd'])) endTest(0)
    else endTest(-1)
}

// Specific event listener used for storage 24 passwords
function wb_receiveNotification_fillStore(event){
    if(!("TextDecoder" in window)){
        alert("This browser does not support TextDecoder")
    }
    var encoder = new TextDecoder("utf-8")
    const valueReceived = event.target.value
    const value = encoder.decode(valueReceived)

    const response = JSON.parse(value)
    
    if(response.hasOwnProperty('err') && response['err'] == 'ok') {
        storedPasswords++
        document.getElementById('finalMessage').innerHTML = 
            'Stored passwords: ' + storedPasswords + "/" + MAX_STORABLE_PWD
        aux_storeOneMorePwd();
    }else if(response.hasOwnProperty('err') && response['err'] == 'storage is full'){
        if(storedPasswords == MAX_STORABLE_PWD) endTest(0)
        else endTest(-1)
    }else endTest(-1)
}

// AUXILIAR FUNCTIONS
// Sends packet to GATT server
function wb_sendPackets(characteristic, packets, packetNumber){
    if(packets[packetNumber] != null){
        characteristic.writeValue(JSONToArray(packets[packetNumber]))
        .then( _ =>{
            if(packetNumber < packets.length){
                wb_sendPackets(characteristic, packets, packetNumber + 1)
            }else{
                console.log("Message sent")
            }
        })
        .catch(error => {console.log("Send packet error: " + error)})
    }
}

function infoToJSON(url, user, pwd){
    if(pwd == null){
        return JSON.stringify({url: url, user: user})
    }else{
        return JSON.stringify({url: url, user: user, pwd: pwd})
    }    
}

function JSONToArray(jsonText){
    if(!("TextDecoder" in window)){
        alert("This browser does not support TextDecoder");
    }
    
    var enc = new TextEncoder()
    var r = enc.encode(jsonText)
    return r
}

function splitString(s, size){
    let numPackets = s.length/size
    let substrings = new Array()
    let position = 0

    for(let i = 0; i < numPackets; i++){
        let newPacket = s.substr(position, MAX_PACKET_SIZE)
        substrings.push(newPacket)
        position += newPacket.length
    }

    return substrings
}